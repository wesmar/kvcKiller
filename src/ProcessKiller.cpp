// ProcessKiller.cpp
// Process termination via driver IOCTL and service-based restoration.
// © WESMAR Marek Wesołowski

#include "ProcessKiller.h"
#include "ConfigReader.h"
#include "StrUtils.h"
#include "KvcContainers.h"

#include <tlhelp32.h>
#include <winsvc.h>
#include <shellapi.h>

// Protects ConfigReader::WriteProcessPath calls inside FindProcesses so that
// parallel refreshes don't race on the registry write.
static SRWLOCK g_cacheLock = SRWLOCK_INIT;

namespace {

WString ToLower(WString s) {
    for (size_t i = 0; i < s.size(); ++i) {
        s[i] = StrUtils::CharLowerW(s[i]);
    }
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// Batch restore
// ---------------------------------------------------------------------------

// Attempts to start each process in exeNames via SCM or ShellExecute fallback.
// results[i] is set to:
//   0 — failed (process not found as a service and no known executable path)
//   1 — successfully started
//   2 — was already running
void ProcessKiller::RestoreProcesses(const Vector<WString>& exeNames,
                                     Vector<int>& results) {
    results.assign(exeNames.size(), 0);

    // Step 1: mark processes that are already running so we don't try to start them.
    auto initiallyRunning = FindProcesses(exeNames);
    Set<WString> runningNames;
    for (const auto& proc : initiallyRunning) {
        runningNames.insert(ToLower(proc.name));
    }

    for (size_t i = 0; i < exeNames.size(); ++i) {
        WString lowerTarget = ToLower(exeNames[i]);
        if (runningNames.contains(lowerTarget)) {
            results[i] = 2; // already running — nothing to do
        }
    }

    // Step 2: enumerate all Win32 services once and build a searchable cache.
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr,
                                     SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_CONNECT);
    Vector<ServiceCacheEntry> serviceCache;

    if (hSCM) {
        DWORD bytesNeeded       = 0;
        DWORD servicesReturned  = 0;
        DWORD resumeHandle      = 0;

        // First call with a null buffer to get the required buffer size.
        EnumServicesStatusExW(hSCM, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
            nullptr, 0, &bytesNeeded, &servicesReturned, &resumeHandle, nullptr);

        if (GetLastError() == ERROR_MORE_DATA) {
            Vector<BYTE> buffer;
            buffer.resize(bytesNeeded);
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
                            Vector<BYTE> configBuffer;
                            configBuffer.resize(configBytesNeeded);
                            auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(configBuffer.data());
                            if (QueryServiceConfigW(hService, config, configBytesNeeded, &configBytesNeeded)) {
                                serviceCache.push_back({ WString(services[i].lpServiceName),
                                                         WString(config->lpBinaryPathName) });
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

            WString lowerTarget = ToLower(exeNames[i]);

            for (const auto& service : serviceCache) {
                WString lowerBinPath = ToLower(service.binaryPath);

                if (lowerBinPath.find(lowerTarget.c_str()) != WString::npos) {
                    SC_HANDLE hService = OpenServiceW(hSCM, service.serviceName.c_str(),
                                                      SERVICE_START);
                    if (hService) {
                        // ERROR_SERVICE_ALREADY_RUNNING is a success
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

    // Step 4: for anything not resolved as a service, launch the stored
    // executable path directly via CreateProcess with STARTF_FORCEOFFFEEDBACK
    // to prevent Windows from displaying the spinning app-starting wait cursor.
    for (size_t i = 0; i < exeNames.size(); ++i) {
        if (results[i] != 0) continue;

        WString lowerTarget = ToLower(exeNames[i]);

        const WString executablePath = ConfigReader::ReadProcessPath(lowerTarget);
        if (!executablePath.empty()) {
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW | STARTF_FORCEOFFFEEDBACK;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, const_cast<wchar_t*>(executablePath.c_str()),
                               nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                results[i] = 1;
            }
        }
    }

    // Signal input idle to instantly drop any pending app-starting cursor feedback.
    MsgWaitForMultipleObjects(0, nullptr, FALSE, 0, QS_ALLINPUT);
}

// Convenience wrapper around RestoreProcesses for single-process callers (CLI).
int ProcessKiller::RestoreProcess(const WString& exeName) {
    Vector<WString> names;
    names.push_back(exeName);
    Vector<int> res;
    RestoreProcesses(names, res);
    return res.empty() ? 0 : res[0];
}

// ---------------------------------------------------------------------------
// Process enumeration
// ---------------------------------------------------------------------------

// Snapshots all running processes and returns those matching processNames.
Vector<ProcessInfo> ProcessKiller::FindProcesses(const Vector<WString>& processNames) {
    Vector<ProcessInfo> found;

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        return found;
    }

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(hSnapshot, &pe)) {
        do {
            WString exeName = ToLower(WString(pe.szExeFile));

            for (const auto& target : processNames) {
                WString targetLower = ToLower(target);

                if (exeName == targetLower) {
                    ProcessInfo info;
                    info.pid     = pe.th32ProcessID;
                    info.name    = exeName;
                    info.running = true;

                    // Refresh the stored path every time the process is seen running
                    {
                        SrwLockGuard lock(g_cacheLock);
                        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                                   FALSE, pe.th32ProcessID);
                        if (hProc) {
                            wchar_t pathBuf[MAX_PATH];
                            DWORD   size = MAX_PATH;
                            if (QueryFullProcessImageNameW(hProc, 0, pathBuf, &size))
                                ConfigReader::WriteProcessPath(targetLower, WString(pathBuf));
                            CloseHandle(hProc);
                        }
                    }

                    found.push_back(info);
                    break;
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

bool ProcessKiller::IsProcessRunning(DWORD pid) {
    HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (hProcess == nullptr) {
        return GetLastError() == ERROR_ACCESS_DENIED;
    }

    const DWORD waitResult = WaitForSingleObject(hProcess, 0);
    CloseHandle(hProcess);

    return waitResult != WAIT_OBJECT_0;
}

// ---------------------------------------------------------------------------
// IOCTL dispatch
// ---------------------------------------------------------------------------

static constexpr DWORD IOCTL_TERMINATOR_HANDSHAKE = 0x00222008;
static constexpr DWORD IOCTL_TERMINATOR_KILL = 0x00222018;
static constexpr DWORD TERMINATOR_MAGIC_ALIVE = 0x77777777;

bool ProcessKiller::InitializeTerminator(HANDLE hDriver) {
    if (hDriver == INVALID_HANDLE_VALUE) return false;

    DWORD input = TERMINATOR_MAGIC_ALIVE;
    DWORD output = 0;
    DWORD returned = 0;
    if (!DeviceIoControl(hDriver, IOCTL_TERMINATOR_HANDSHAKE,
                         &input, sizeof(input),
                         &output, sizeof(output),
                         &returned, nullptr)) {
        return false;
    }

    return returned >= sizeof(DWORD) && output == 1;
}

bool ProcessKiller::SendIOCTL(HANDLE hDriver, DWORD pid) {
    if (hDriver == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD status = 0;
    DWORD returned = 0;
    const BOOL accepted = DeviceIoControl(
        hDriver,
        IOCTL_TERMINATOR_KILL,
        &pid,
        sizeof(pid),
        &status,
        sizeof(status),
        &returned,
        nullptr
    );

    return accepted != FALSE
        && returned >= sizeof(status)
        && static_cast<LONG>(status) >= 0;
}

bool ProcessKiller::KillProcess(HANDLE hDriver, DWORD pid) {
    return SendIOCTL(hDriver, pid);
}

Vector<WString> ProcessKiller::GetDefaultTargetList() {
    return ConfigReader::ReadSeedProcessList();
}
