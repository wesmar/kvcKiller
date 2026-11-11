// Utils.cpp
// © WESMAR Marek Wesołowski
//
// General-purpose utilities shared across the whole codebase:
//   - String resource loading.
//   - OS / environment detection (Windows version, admin check).
//   - String helpers (lowercase copy, extension strip, normalization).
//   - Thread-safe per-process state accessors (last action, running state).
//   - Cross-thread UI helpers (SetButtonsEnabled).

#include "Utils.h"
#include "GlobalData.h"
#include "Resource.h"
#include "StrUtils.h"

// ---------------------------------------------------------------------------
// String resources
// ---------------------------------------------------------------------------

// Loads a string from the module's embedded STRINGTABLE resource.
// The 512-character buffer is large enough for all UI strings in practice;
// truncation is silently accepted (LoadStringW null-terminates on overflow).
WString LoadStr(UINT id) {
    wchar_t buf[512] = {};
    LoadStringW(GetModuleHandleW(nullptr), id, buf, static_cast<int>(sizeof(buf) / sizeof(buf[0])));
    return WString(buf);
}

// ---------------------------------------------------------------------------
// OS / environment detection
// ---------------------------------------------------------------------------

// Returns true if the running OS is Windows 11 or later (build >= 22000).
// Used to select the correct font (Segoe UI Variable) and to opt into
// Windows 11-only DWM features (Mica backdrop via DWMWA_SYSTEMBACKDROP_TYPE).
bool IsWindows11OrLater() {
    HKEY hKey;
    DWORD dwType = 0, dwSize = 0;
    wchar_t build[32] = {};

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                      0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;

    dwSize = sizeof(build);
    const bool ok = RegQueryValueExW(hKey, L"CurrentBuildNumber", nullptr, &dwType,
                                     reinterpret_cast<LPBYTE>(build), &dwSize) == ERROR_SUCCESS;
    RegCloseKey(hKey);
    // Build 22000 is the first Windows 11 release (21H2).
    return ok && StrUtils::StrToUint(build) >= 22000;
}

// Returns true if the current process token has membership in the local
// Administrators group.
//
// CheckTokenMembership is used (instead of IsUserAnAdmin / IsInGroup) because
// it correctly handles UAC: if the process is elevated, the Administrators SID
// is active in the token and the check returns TRUE; if running unelevated it
// returns FALSE even if the user is a member, which is the correct answer for
// our purposes (we need actual elevation, not just group membership).
bool IsRunningAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;

    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuthority, 2,
                                  SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                  0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin != FALSE;
}

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------

// Returns a lowercase copy of value using StrUtils::CharLowerW.
// Used for case-insensitive key normalization throughout the codebase.
WString ToLowerCopy(WString value) {
    for (size_t i = 0; i < value.size(); ++i) {
        value[i] = StrUtils::CharLowerW(value[i]);
    }
    return value;
}

// Removes a trailing ".exe" suffix (case-insensitive) from value.
// Input and output preserve the original case of the non-suffix characters.
// Example: "MsMpEng.EXE" → "MsMpEng"
WString StripExeExtension(const WString& value) {
    WString lower = ToLowerCopy(value);
    if (lower.length() > 4 && lower.substr(lower.length() - 4) == L".exe")
        return value.substr(0, value.length() - 4);
    return value;
}

// Ensures value ends with ".exe", adding the suffix if it is missing.
// Always lower-cases the extension for consistency.
// Example: "MsMpEng" → "MsMpEng.exe"
WString EnsureExeName(const WString& value) {
    return StripExeExtension(value) + L".exe";
}

// Produces the canonical map key for a process: lowercase, no .exe extension.
// All g_lastActionByName / g_prevRunningByName lookups use this key so that
// "MsMpEng.exe", "msmpeng.EXE", and "MsMpEng" all map to the same entry.
WString NormalizeProcessKey(const WString& value) {
    return ToLowerCopy(StripExeExtension(value));
}

// Returns pid formatted as a decimal string, or "-" for PID 0 (not running).
WString FormatPid(DWORD pid) {
    if (pid == 0) return WString(L"-");
    wchar_t buffer[32];
    StrUtils::UintToStr(pid, buffer, sizeof(buffer) / sizeof(buffer[0]));
    return WString(buffer);
}

// ---------------------------------------------------------------------------
// Thread-safe per-process state
// ---------------------------------------------------------------------------
// All four functions below acquire g_statusLock (a CRITICAL_SECTION) for
// every read and write.  They are called from both the UI thread and worker
// threads so the lock is mandatory.
//
// kActionNone / empty is stored as absence from the map to keep the map
// small — most processes have no pending action at any given time.

// Stores (or clears) the last user-visible action label for a process.
// Passing kActionNone or an empty string removes the entry so ReadLastAction
// returns kActionNone without a map hit.
void StoreLastAction(const WString& processName, const WString& action) {
    const WString key = NormalizeProcessKey(processName);
    if (key.empty()) return;

    EnterCriticalSection(&g_statusLock);
    if (action == kActionNone || action.empty())
        g_lastActionByName.erase(key);
    else
        g_lastActionByName[key] = action;
    LeaveCriticalSection(&g_statusLock);
}

// Returns the last stored action label for a process, or kActionNone if no
// entry exists.
WString ReadLastAction(const WString& processName) {
    const WString key = NormalizeProcessKey(processName);

    EnterCriticalSection(&g_statusLock);
    const auto it = g_lastActionByName.find(key);
    const WString action = (it != g_lastActionByName.end()) ? it->second : WString(kActionNone);
    LeaveCriticalSection(&g_statusLock);
    return action;
}

// Returns the running state recorded during the PREVIOUS RefreshProcessList tick.
// Used by the action-label state machine to detect transitions:
//   not-running → running  = "restarted"
//   running → not-running  = clears pending "restored" label
// Returns false if no prior state has been recorded (first tick).
bool WasRunningPreviously(const WString& processName) {
    const WString key = NormalizeProcessKey(processName);

    EnterCriticalSection(&g_statusLock);
    const auto it = g_prevRunningByName.find(key);
    const bool wasRunning = (it != g_prevRunningByName.end()) ? it->second : false;
    LeaveCriticalSection(&g_statusLock);
    return wasRunning;
}

// Records the current running state for comparison on the next refresh tick.
// Called at the end of each row's processing in RefreshProcessList.
void StoreRunningState(const WString& processName, bool running) {
    const WString key = NormalizeProcessKey(processName);
    if (key.empty()) return;

    EnterCriticalSection(&g_statusLock);
    g_prevRunningByName[key] = running;
    LeaveCriticalSection(&g_statusLock);
}

// ---------------------------------------------------------------------------
// Cross-thread UI helpers
// ---------------------------------------------------------------------------

// Posts WMU_SET_BUTTONS to the main window from any thread.
// WndProc handles it on the UI thread by calling EnableWindow on both buttons.
// PostMessage is non-blocking so this is safe to call from worker threads.
void SetButtonsEnabled(bool enabled) {
    PostMessage(hInstMain, WMU_SET_BUTTONS, enabled ? 1 : 0, 0);
}
