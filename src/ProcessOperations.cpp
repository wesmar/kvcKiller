// ProcessOperations.cpp
// © WESMAR Marek Wesołowski
//
// High-level GUI actions: KillOnceAction, RestoreAction, and the worker threads
// they spawn.  Also owns Paralyze/Unparalyze logic via offline IFEO registry injection,
// and the driver initialisation helper.

#include "ProcessOperations.h"
#include "GlobalData.h"
#include "Utils.h"
#include "UIHelpers.h"
#include "ListViewManager.h"
#include "DriverController.h"
#include "ProcessKiller.h"
#include "ConfigReader.h"
#include "Resource.h"

#include <set>
#include <vector>
#include <iostream>

namespace {

// ---------------------------------------------------------------------------
// Privilege setup for registry operations
// ---------------------------------------------------------------------------

static void EnablePrivilege(HANDLE hToken, LPCWSTR name) {
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    if (LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid)) {
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    }
}

static void EnableParalyzePrivileges() {
    HANDLE hTok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(),
                         TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hTok)) {
        EnablePrivilege(hTok, SE_BACKUP_NAME);
        EnablePrivilege(hTok, SE_RESTORE_NAME);
        CloseHandle(hTok);
    }
}

static std::wstring GetWindowsTempPath() {
    wchar_t path[MAX_PATH];
    UINT result = GetWindowsDirectoryW(path, MAX_PATH);
    if (result == 0 || result > MAX_PATH) return L"";
    return std::wstring(path) + L"\\temp\\";
}

static void CleanupTempFiles(const std::wstring& basePath) {
    std::vector<std::wstring> patterns = {
        basePath + L"Ifeo.hiv",
        basePath + L"Ifeo.hiv.LOG1", 
        basePath + L"Ifeo.hiv.LOG2",
        basePath + L"Ifeo.hiv.blf"
    };
    
    for (const auto& file : patterns) {
        DeleteFileW(file.c_str());
    }
    
    WIN32_FIND_DATAW findData;
    std::wstring searchPattern = basePath + L"*.regtrans-ms";
    HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &findData);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            std::wstring fullPath = basePath + findData.cFileName;
            DeleteFileW(fullPath.c_str());
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
    }
}

// Core registry batch processing function
static int ProcessBatchIfeo(const std::vector<std::wstring>& processNames, bool paralyze) {
    if (processNames.empty()) return 0;

    EnableParalyzePrivileges();

    HKEY hkcuKvc;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\kvckiller", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &hkcuKvc, NULL) != ERROR_SUCCESS) {
        return 0;
    }

    HKEY tempCheck;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"TempIFEO", 0, KEY_READ, &tempCheck) == ERROR_SUCCESS) {
        RegCloseKey(tempCheck);
        RegUnLoadKeyW(HKEY_LOCAL_MACHINE, L"TempIFEO");
    }

    std::wstring tempPath = GetWindowsTempPath();
    if (tempPath.empty()) {
        RegCloseKey(hkcuKvc);
        return 0;
    }
    
    CreateDirectoryW(tempPath.c_str(), nullptr);
    std::wstring hiveFile = tempPath + L"Ifeo.hiv";

    if (GetFileAttributesW(hiveFile.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(hiveFile.c_str());
    }

    HKEY ifeoKey;
    LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options", 0, KEY_READ, &ifeoKey);
    if (result != ERROR_SUCCESS) {
        RegCloseKey(hkcuKvc);
        return 0;
    }

    result = RegSaveKeyExW(ifeoKey, hiveFile.c_str(), nullptr, REG_LATEST_FORMAT);
    RegCloseKey(ifeoKey);
    if (result != ERROR_SUCCESS) {
        RegCloseKey(hkcuKvc);
        return 0;
    }

    if (RegLoadKeyW(HKEY_LOCAL_MACHINE, L"TempIFEO", hiveFile.c_str()) != ERROR_SUCCESS) {
        RegCloseKey(hkcuKvc);
        return 0;
    }

    int successCount = 0;
    std::wstring debugger = L"C:\\Windows\\System32\\systray.exe";

    for (const auto& name : processNames) {
        std::wstring exeName = name + L".exe";
        std::wstring subKeyPath = L"TempIFEO\\" + exeName;
        HKEY subKey;
        
        if (paralyze) {
            bool existed = (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKeyPath.c_str(), 0, KEY_READ, &subKey) == ERROR_SUCCESS);
            if (existed) {
                RegCloseKey(subKey);
            } else {
                DWORD val = 1;
                RegSetValueExW(hkcuKvc, exeName.c_str(), 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
            }

            result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, subKeyPath.c_str(), 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &subKey, NULL);
            if (result == ERROR_SUCCESS) {
                result = RegSetValueExW(subKey, L"Debugger", 0, REG_SZ, (const BYTE*)debugger.c_str(), (DWORD)((debugger.length() + 1) * sizeof(wchar_t)));
                if (result == ERROR_SUCCESS) ++successCount;
                RegCloseKey(subKey);
            }
        } else {
            DWORD type = 0, val = 0;
            DWORD size = sizeof(val);
            bool createdByUs = (RegQueryValueExW(hkcuKvc, exeName.c_str(), NULL, &type, (LPBYTE)&val, &size) == ERROR_SUCCESS && val == 1);

            if (createdByUs) {
                LSTATUS delRes = RegDeleteKeyW(HKEY_LOCAL_MACHINE, subKeyPath.c_str());
                if (delRes == ERROR_SUCCESS || delRes == ERROR_FILE_NOT_FOUND) {
                    ++successCount;
                    RegDeleteValueW(hkcuKvc, exeName.c_str());  // only when delete succeeded
                }
            } else {
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKeyPath.c_str(), 0, KEY_WRITE, &subKey) == ERROR_SUCCESS) {
                    LSTATUS delRes = RegDeleteValueW(subKey, L"Debugger");
                    if (delRes == ERROR_SUCCESS || delRes == ERROR_FILE_NOT_FOUND) {
                        ++successCount;
                    }
                    RegCloseKey(subKey);
                } else {
                    ++successCount; // Not found, effectively unparalyzed
                }
            }
        }
    }

    RegCloseKey(hkcuKvc);
    RegUnLoadKeyW(HKEY_LOCAL_MACHINE, L"TempIFEO");

    if (successCount > 0) {
        result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options", 0, KEY_WRITE, &ifeoKey);
        if (result == ERROR_SUCCESS) {
            RegRestoreKeyW(ifeoKey, hiveFile.c_str(), REG_FORCE_RESTORE);
            RegCloseKey(ifeoKey);
        } else {
            successCount = 0; // Restore failed
        }
    }

    CleanupTempFiles(tempPath);
    return successCount;
}

// ---------------------------------------------------------------------------
// Kill thread parameter block
// ---------------------------------------------------------------------------

// Carries UI state from KillOnceAction (UI thread) to KillOnceThreadProc.
// Reading BM_GETCHECK here avoids a cross-thread SendMessage in the worker.
struct KillThreadParams {
    bool paralyze; // true if the Paralyze checkbox was ticked when Kill was clicked
};

// ---------------------------------------------------------------------------
// Worker lifetime tracking
// ---------------------------------------------------------------------------

// Called on every exit path of a worker thread.  Decrements g_activeWorkers;
// if this was the last active worker and a shutdown is pending, re-posts
// WM_CLOSE so WndProc can finally call DestroyWindow.
static void NotifyWorkerDone() {
    if (g_activeWorkers.fetch_sub(1) == 1 && g_shutdownPending.load())
        PostMessage(hInstMain, WM_CLOSE, 0, 0);
}

// RAII guard that calls NotifyWorkerDone in its destructor.
// Place one at the top of every worker thread proc so the decrement is
// guaranteed regardless of how many return paths the function has.
struct WorkerGuard {
    ~WorkerGuard() { NotifyWorkerDone(); }
};

} // namespace

// ---------------------------------------------------------------------------
// Paralyze / unparalyze (public)
// ---------------------------------------------------------------------------

int ParalyzeProcessesBatch(const std::vector<std::wstring>& processNames) {
    return ProcessBatchIfeo(processNames, true);
}

int UnparalyzeProcessesBatch(const std::vector<std::wstring>& processNames) {
    return ProcessBatchIfeo(processNames, false);
}

bool IsProcessParalyzed(const std::wstring& processName) {
    std::wstring exeName = processName + L".exe";
    std::wstring keyPath = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\" + exeName;
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD type, size;
        bool isParalyzed = (RegQueryValueExW(hKey, L"Debugger", nullptr, &type, nullptr, &size) == ERROR_SUCCESS);
        RegCloseKey(hKey);
        return isParalyzed;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Driver lifecycle
// ---------------------------------------------------------------------------

// Ensures the kernel driver is loaded and ready to accept IOCTLs.
// Called at the start of every Kill action before opening the driver handle.
// Idempotent: returns true immediately if the driver is already loaded.
bool InitializeDriver() {
    if (DriverController::IsDriverLoaded()) return true;

    const std::wstring driverPath = DriverController::GetDriverPath();
    if (!DriverController::RegisterDriver(driverPath)) return false;
    if (!DriverController::StartDriver())              return false;
    return true;
}

// ---------------------------------------------------------------------------
// Kill worker thread
// ---------------------------------------------------------------------------

// Runs on a private thread; performs the actual IOCTL kill sequence.
// WorkerGuard at the top guarantees g_activeWorkers is decremented on every
// exit path, including early returns.
DWORD WINAPI KillOnceThreadProc(LPVOID lpParam) {
    WorkerGuard guard{};

    // Claim the params block allocated by KillOnceAction on the UI thread.
    bool paralyzeEnabled = false;
    if (lpParam) {
        auto* p     = static_cast<KillThreadParams*>(lpParam);
        paralyzeEnabled = p->paralyze;
        delete p;
    }

    // If the selection is empty, restore the UI state and exit cleanly.
    auto selected = GetSelectedProcesses();
    if (selected.empty()) {
        if (!g_shutdownPending.load()) UpdateStatusText(LoadStr(IDS_STATUS_READY));
        SetButtonsEnabled(true);
        DriverController::CloseDriverHandle();
        DriverController::CleanupDriverRegistry();
        return 0;
    }

    HANDLE hDriver = DriverController::GetDriverHandle();
    if (hDriver == INVALID_HANDLE_VALUE) {
        if (!g_shutdownPending.load()) UpdateStatusText(LoadStr(IDS_STATUS_DRIVER_ERR));
        SetButtonsEnabled(true);
        DriverController::CleanupDriverRegistry();
        return 0;
    }

    // Build the .exe name list for FindProcesses.
    std::vector<std::wstring> exeNames;
    for (const auto& name : selected) exeNames.push_back(name + L".exe");

    auto running = ProcessKiller::FindProcesses(exeNames);
    if (running.empty()) {
        // All selected processes were already dead.
        UpdateStatusText(LoadStr(IDS_STATUS_ALREADY_DEAD));
        SetButtonsEnabled(true);
    } else {
        int killedCount       = 0;
        int failedCount       = 0;
        int paralyzeFailCount = 0;

        std::vector<std::wstring> killedNames; // names without .exe, for batch paralyze

        for (const auto& proc : running) {
            if (ProcessKiller::KillProcess(hDriver, proc.pid)) {
                Sleep(300); // brief wait to let the process exit before polling
                if (!ProcessKiller::IsProcessRunning(proc.pid)) {
                    ++killedCount;
                    StoreLastAction(proc.name, kActionKilled);
                    const std::wstring path = ConfigReader::ReadProcessPath(proc.name);
                    ConfigReader::RecordHistory(L"KILL", proc.name, path);
                    // proc.name is always "foo.exe" (lower-cased by FindProcesses)
                    killedNames.push_back(proc.name.substr(0, proc.name.size() - 4));
                } else {
                    // IOCTL sent but process survived — likely protected.
                    ++failedCount;
                }
            } else {
                // DeviceIoControl itself failed.
                ++failedCount;
            }
        }

        if (paralyzeEnabled && !killedNames.empty()) {
            int paralyzed     = ParalyzeProcessesBatch(killedNames);
            paralyzeFailCount = static_cast<int>(killedNames.size()) - paralyzed;
            // Promote KILLED → PARALYZED for processes where IFEO was set.
            // killedNames are already normalized (lowercase, no .exe).
            for (int i = 0; i < paralyzed; ++i)
                StoreLastAction(killedNames[i], kActionParalyzed);
        }

        // Update the status label based on outcome.
        // Skip UI updates if the window is already being destroyed.
        if (g_shutdownPending.load()) {
            // window gone — skip all UI updates
        } else if (killedCount > 0 && failedCount == 0 && paralyzeFailCount == 0)
            UpdateStatusText(LoadStr(IDS_STATUS_KILL_ALL_OK));
        else if (killedCount > 0)
            UpdateStatusText(LoadStr(IDS_STATUS_KILL_PARTIAL));
        else
            UpdateStatusText(LoadStr(IDS_STATUS_KILL_FAILED));

        if (!g_shutdownPending.load()) PostMessage(hInstMain, WMU_REFRESH_LIST, 0, 0);
        SetButtonsEnabled(true);
    }

    // Always release the driver handle and clean up registry traces after the
    // operation, even on failure paths.
    DriverController::CloseDriverHandle();
    DriverController::CleanupDriverRegistry();
    return 0;
}

// ---------------------------------------------------------------------------
// Restore worker thread
// ---------------------------------------------------------------------------

// Runs on a private thread; unparalyzes and starts each selected process.
// WorkerGuard ensures the active-worker count is decremented on all exits.
DWORD WINAPI RestoreThreadProc(LPVOID) {
    WorkerGuard guard{};

    auto selected = GetSelectedProcesses();
    if (selected.empty()) {
        UpdateStatusText(LoadStr(IDS_STATUS_READY));
        SetButtonsEnabled(true);
        return 0;
    }

    // Identify which selected processes are already running (result code 2 later).
    std::vector<std::wstring> exeNames;
    for (const auto& name : selected) exeNames.push_back(name + L".exe");

    std::set<std::wstring> initiallyRunning;
    for (const auto& proc : ProcessKiller::FindProcesses(exeNames))
        initiallyRunning.insert(NormalizeProcessKey(proc.name));

    // Collect processes that need restoring and mark them with a transient
    // "restoring" label so the ListView shows immediate feedback.
    std::vector<std::wstring> pendingNames;
    for (const auto& name : selected) {
        if (initiallyRunning.contains(NormalizeProcessKey(name))) continue;
        pendingNames.push_back(name);
        StoreLastAction(name, kActionRestoring);
    }

    if (!pendingNames.empty()) {
        if (!g_shutdownPending.load()) PostMessage(hInstMain, WMU_REFRESH_LIST, 0, 0);

        // --- Unparalyze Phase (BEFORE Restore) ---
        // Identify processes that need to be unparalyzed (they have Debugger set in IFEO).
        std::vector<std::wstring> toUnparalyze;
        for (const auto& name : pendingNames) {
            if (IsProcessParalyzed(name)) {
                toUnparalyze.push_back(name);
            }
        }

        int unparalyzeFailCount = 0;
        if (!toUnparalyze.empty()) {
            int success = UnparalyzeProcessesBatch(toUnparalyze);
            unparalyzeFailCount = static_cast<int>(toUnparalyze.size()) - success;
        }

        // Build the .exe names list for RestoreProcesses.
        std::vector<std::wstring> pendingExe;
        for (const auto& name : pendingNames) pendingExe.push_back(name + L".exe");

        std::vector<int> results;
        ProcessKiller::RestoreProcesses(pendingExe, results);

        int restoredCount = 0;
        int failedCount   = 0;
        for (size_t i = 0; i < pendingNames.size(); ++i) {
            if (results[i] == 1 || results[i] == 2) {
                StoreLastAction(pendingNames[i], kActionRestored);
                ConfigReader::RecordHistory(L"RESTORE", pendingNames[i],
                                            ConfigReader::ReadProcessPath(pendingNames[i]));
                ++restoredCount;
            } else {
                StoreLastAction(pendingNames[i], kActionNone);
                ++failedCount;
            }
        }

        if (g_shutdownPending.load()) {
            // window gone — skip all UI updates
        } else if (restoredCount > 0 && failedCount == 0 && unparalyzeFailCount == 0)
            UpdateStatusText(LoadStr(IDS_STATUS_RESTORE_OK));
        else if (restoredCount > 0)
            UpdateStatusText(LoadStr(IDS_STATUS_RESTORE_PARTIAL));
        else
            UpdateStatusText(LoadStr(IDS_STATUS_RESTORE_FAILED));

    } else {
        // Every selected process was already running.
        if (!g_shutdownPending.load()) UpdateStatusText(LoadStr(IDS_STATUS_ALREADY_RUNNING));
    }

    if (!g_shutdownPending.load()) PostMessage(hInstMain, WMU_REFRESH_LIST, 0, 0);
    SetButtonsEnabled(true);
    return 0;
}

// ---------------------------------------------------------------------------
// GUI action triggers  (called from WM_COMMAND on the UI thread)
// ---------------------------------------------------------------------------

// Initiates a kill operation.  All blocking work happens in KillOnceThreadProc.
//
// The Paralyze checkbox state is read here on the UI thread and passed via
// KillThreadParams so the worker never needs to call SendMessage across threads
// (which would be a deadlock if the UI thread is blocked waiting for the worker).
//
// g_activeWorkers is incremented BEFORE CreateThread so that WM_CLOSE on the
// UI thread immediately sees a non-zero count and defers DestroyWindow.
void KillOnceAction() {
    if (g_shutdownPending.load()) return;
    if (!IsRunningAsAdmin()) {
        MessageBoxW(nullptr, LoadStr(IDS_ERR_NEED_ADMIN).c_str(),
                    LoadStr(IDS_ERR_CAPTION).c_str(), MB_ICONERROR);
        return;
    }
    if (!InitializeDriver()) return;

    UpdateStatusText(LoadStr(IDS_STATUS_KILLING));
    EnableWindow(hKillOnceButton, FALSE);
    EnableWindow(hRestoreButton,  FALSE);

    auto* params = new KillThreadParams{
        SendMessageW(hParalyzeCheck, BM_GETCHECK, 0, 0) == BST_CHECKED
    };

    // Increment before CreateThread — see threading model note at top of file.
    g_activeWorkers.fetch_add(1);
    HANDLE hThread = CreateThread(nullptr, 0, KillOnceThreadProc, params, 0, nullptr);
    if (hThread) {
        CloseHandle(hThread); // we don't need to join — WorkerGuard handles cleanup
    } else {
        delete params;
        NotifyWorkerDone(); // thread never started — balance the increment
    }
}

// Initiates a restore operation.  All blocking work happens in RestoreThreadProc.
// No params struct needed — the worker reads the selection list itself via
// GetSelectedProcesses (which only accesses g_visibleRows, not cross-thread UI state).
void RestoreAction() {
    if (g_shutdownPending.load()) return;
    if (!IsRunningAsAdmin()) {
        MessageBoxW(nullptr, LoadStr(IDS_ERR_NEED_ADMIN).c_str(),
                    LoadStr(IDS_ERR_CAPTION).c_str(), MB_ICONERROR);
        return;
    }

    UpdateStatusText(LoadStr(IDS_STATUS_RESTORING_SVC));
    SetButtonsEnabled(false);

    // Increment before CreateThread — same reasoning as KillOnceAction.
    g_activeWorkers.fetch_add(1);
    HANDLE hThread = CreateThread(nullptr, 0, RestoreThreadProc, nullptr, 0, nullptr);
    if (hThread) {
        CloseHandle(hThread);
    } else {
        NotifyWorkerDone(); // thread never started — balance the increment
    }
}
