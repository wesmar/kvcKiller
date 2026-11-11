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
#include "KillEngine.h"
#include "ProcessKiller.h"
#include "ConfigReader.h"
#include "Resource.h"
#include "StrUtils.h"
#include "KvcContainers.h"

#include <tlhelp32.h>

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

static WString GetWindowsTempPath() {
    wchar_t path[MAX_PATH];
    UINT result = GetWindowsDirectoryW(path, MAX_PATH);
    if (result == 0 || result > MAX_PATH) return WString();
    return WString(path) + L"\\temp\\";
}

static void CleanupTempFiles(const WString& basePath) {
    WString patterns[4] = {
        basePath + L"Ifeo.hiv",
        basePath + L"Ifeo.hiv.LOG1", 
        basePath + L"Ifeo.hiv.LOG2",
        basePath + L"Ifeo.hiv.blf"
    };
    
    for (const auto& file : patterns) {
        DeleteFileW(file.c_str());
    }
    
    WIN32_FIND_DATAW findData;
    WString searchPattern = basePath + L"*.regtrans-ms";
    HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &findData);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            WString fullPath = basePath + findData.cFileName;
            DeleteFileW(fullPath.c_str());
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
    }
}

// Core registry batch processing function
static int ProcessBatchIfeo(const Vector<WString>& processNames, bool paralyze) {
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

    WString tempPath = GetWindowsTempPath();
    if (tempPath.empty()) {
        RegCloseKey(hkcuKvc);
        return 0;
    }

    CreateDirectoryW(tempPath.c_str(), nullptr);
    WString hiveFile = tempPath + L"Ifeo.hiv";

    if (GetFileAttributesW(hiveFile.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(hiveFile.c_str());
    }

    HKEY ifeoKey;
    LSTATUS result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options", 0, KEY_READ, &ifeoKey);
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
    WString debugger = L"C:\\Windows\\System32\\systray.exe";

    for (const auto& name : processNames) {
        WString exeName = EnsureExeName(name);
        WString subKeyPath = L"TempIFEO\\" + exeName;
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

bool StopServiceAndWait(std::wstring_view serviceName, DWORD timeoutMs) {
    SC_HANDLE scm = OpenSCManagerW(
        nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;

    WString name(serviceName);
    SC_HANDLE queryService = OpenServiceW(
        scm, name.c_str(), SERVICE_QUERY_STATUS);
    if (!queryService) {
        CloseServiceHandle(scm);
        return false;
    }

    SERVICE_STATUS_PROCESS status{};
    DWORD needed = 0;
    auto query = [&] {
        return QueryServiceStatusEx(
            queryService, SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&status), sizeof(status), &needed);
    };

    bool stopped = false;
    if (query() && status.dwCurrentState == SERVICE_STOPPED) {
        stopped = true;
    } else {
        // SERVICE_QUERY_STATUS may be granted while SERVICE_STOP is denied by
        // a protected service. Request stop rights only when actually needed.
        SC_HANDLE stopService = OpenServiceW(
            scm, name.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS);
        if (stopService) {
            SERVICE_STATUS controlStatus{};
            ControlService(stopService, SERVICE_CONTROL_STOP, &controlStatus);
            CloseServiceHandle(stopService);
        }

        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        while (GetTickCount64() < deadline) {
            if (query() && status.dwCurrentState == SERVICE_STOPPED) {
                stopped = true;
                break;
            }
            Sleep(100);
        }
    }

    CloseServiceHandle(queryService);
    CloseServiceHandle(scm);
    return stopped;
}

Set<DWORD> SnapshotProcessIds() {
    Set<DWORD> pids;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return pids;

    PROCESSENTRY32W entry{sizeof(entry)};
    if (Process32FirstW(snapshot, &entry)) {
        do {
            pids.insert(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pids;
}

bool WaitForInitialPidsToDisappear(
    const Vector<KillTargetReport>& targets,
    DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (true) {
        const auto livePids = SnapshotProcessIds();
        bool anyInitialPidAlive = false;
        for (const auto& target : targets) {
            for (DWORD pid : target.initialPids) {
                if (livePids.contains(pid)) {
                    anyInitialPidAlive = true;
                    break;
                }
            }
            if (anyInitialPidAlive) break;
        }

        if (!anyInitialPidAlive || GetTickCount64() >= deadline) {
            return !anyInitialPidAlive;
        }
        Sleep(50);
    }
}

KillTargetReport* FindTargetReport(
    Vector<KillTargetReport>& targets, std::wstring_view processName) {
    const WString key = NormalizeProcessKey(WString(processName));
    for (auto& target : targets) {
        if (NormalizeProcessKey(target.name) == key) {
            return &target;
        }
    }
    return nullptr;
}

bool AllTrapsSet(const Vector<KillTargetReport>& targets) {
    for (const auto& target : targets) {
        if (!target.trapSet) return false;
    }
    return true;
}

bool AnyTerminated(const Vector<KillTargetReport>& targets) {
    for (const auto& target : targets) {
        if (target.terminated) return true;
    }
    return false;
}

bool AnyRunningAfter(const Vector<KillTargetReport>& targets) {
    for (const auto& target : targets) {
        if (target.runningAfter) return true;
    }
    return false;
}

bool AnyOriginalSurvived(const Vector<KillTargetReport>& targets) {
    for (const auto& target : targets) {
        if (target.originalSurvived) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Kill thread parameter block
// ---------------------------------------------------------------------------

struct KillThreadParams {
    Vector<WString> targets;
    bool paralyze = false;
    bool defenderSet = false;
};

struct RestoreThreadParams {
    Vector<WString> targets;
};

// ---------------------------------------------------------------------------
// Worker lifetime tracking
// ---------------------------------------------------------------------------

static void NotifyWorkerDone() {
    if (g_activeWorkers.fetch_sub(1) == 1 && g_shutdownPending.load())
        PostMessage(hInstMain, WM_CLOSE, 0, 0);
}

struct WorkerGuard {
    ~WorkerGuard() { NotifyWorkerDone(); }
};

} // namespace

// ---------------------------------------------------------------------------
// Paralyze / unparalyze (public)
// ---------------------------------------------------------------------------

int ParalyzeProcessesBatch(const Vector<WString>& processNames) {
    return ProcessBatchIfeo(processNames, true);
}

int UnparalyzeProcessesBatch(const Vector<WString>& processNames) {
    return ProcessBatchIfeo(processNames, false);
}

bool IsProcessParalyzed(const WString& processName) {
    WString exeName = EnsureExeName(processName);
    WString keyPath = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\" + exeName;
    
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD type = 0;
        if (RegQueryValueExW(hKey, L"Debugger", NULL, &type, NULL, NULL) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return true;
        }
        RegCloseKey(hKey);
    }
    return false;
}

Vector<WString> DefenderTargetList() {
    Vector<WString> targets;
    targets.reserve(kDefenderTargets.size());
    for (const std::wstring_view target : kDefenderTargets) {
        targets.emplace_back(target);
    }
    return targets;
}

KillOperationReport ExecuteKillTargets(
    std::span<const WString> targets,
    bool paralyzeFirst,
    bool defenderSet) {
    KillOperationReport report{};
    report.defenderSet = defenderSet;
    report.targets.reserve(targets.size());

    Vector<WString> normalizedTargets;
    Vector<WString> exeNames;
    normalizedTargets.reserve(targets.size());
    exeNames.reserve(targets.size());
    for (const WString& target : targets) {
        KillTargetReport item{};
        item.name = StripExeExtension(target);
        report.targets.push_back(std::move(item));
        normalizedTargets.push_back(report.targets.back().name);
        exeNames.push_back(report.targets.back().name + L".exe");
    }

    const auto initialRunning = ProcessKiller::FindProcesses(exeNames);
    if (!paralyzeFirst && initialRunning.empty()) {
        report.outcome = KillOutcome::AlreadyStopped;
        return report;
    }

    if (!KillEngine::Start()) {
        report.outcome = KillOutcome::Failed;
        return report;
    }

    report.pplReady = KillEngine::HasPplUnprotector();
    report.terminatorReady = KillEngine::HasProcessTerminator();

    if (paralyzeFirst) {
        ParalyzeProcessesBatch(normalizedTargets);
        for (KillTargetReport& target : report.targets) {
            target.trapSet = IsProcessParalyzed(target.name);
        }
    }

    const auto running = ProcessKiller::FindProcesses(exeNames);
    Vector<DWORD> pids;
    pids.reserve(running.size());
    for (const ProcessInfo& process : running) {
        if (KillTargetReport* target = FindTargetReport(
                report.targets, process.name)) {
            if (target->initialPids.empty()) target->pid = process.pid;
            target->initialPids.push_back(process.pid);
            target->wasRunning = true;
        }
        pids.push_back(process.pid);
    }

    if (!pids.empty() && report.pplReady) {
        Vector<PplTargetResult> protection;
        KillEngine::LowerProtection(pids, protection);
    }

    for (const ProcessInfo& process : running) {
        const KillAttemptResult attempt = KillEngine::Kill(process.pid);
        if (KillTargetReport* target = FindTargetReport(
                report.targets, process.name)) {
            target->terminated = target->terminated || attempt.terminated;
        }
    }

    WaitForInitialPidsToDisappear(report.targets, 3000);

    if (defenderSet) {
        report.winDefendStopped = StopServiceAndWait(L"WinDefend", 6000);
        report.securityHealthStopped =
            StopServiceAndWait(L"SecurityHealthService", 4000);
    }

    const auto livePids = SnapshotProcessIds();
    for (KillTargetReport& target : report.targets) {
        bool initialPidAlive = false;
        for (DWORD pid : target.initialPids) {
            if (livePids.contains(pid)) {
                initialPidAlive = true;
                break;
            }
        }
        if (target.wasRunning && !initialPidAlive) {
            target.terminated = true;
        }
        target.originalSurvived = initialPidAlive;
    }

    const auto finalRunning = ProcessKiller::FindProcesses(exeNames);
    for (const ProcessInfo& process : finalRunning) {
        if (!livePids.contains(process.pid)
            || !ProcessKiller::IsProcessRunning(process.pid)) {
            continue;
        }
        if (KillTargetReport* target = FindTargetReport(
                report.targets, process.name)) {
            target->runningAfter = true;
            bool sameProcess = false;
            for (DWORD initPid : target->initialPids) {
                if (initPid == process.pid) {
                    sameProcess = true;
                    break;
                }
            }
            target->originalSurvived =
                target->originalSurvived || sameProcess;
            target->restartedAfter =
                target->restartedAfter || !sameProcess;
        }
    }

    KillEngine::Stop();

    if (defenderSet) {
        const KillTargetReport* mainEngine = FindTargetReport(
            report.targets, L"MsMpEng");
        const bool mainEngineStopped =
            mainEngine && !mainEngine->runningAfter;
        const bool trapsComplete =
            !paralyzeFirst || AllTrapsSet(report.targets);
        const bool complete =
            trapsComplete
            && report.winDefendStopped
            && mainEngineStopped;

        if (complete) {
            report.outcome = KillOutcome::Succeeded;
        } else if (report.winDefendStopped
                   || mainEngineStopped
                   || AnyTerminated(report.targets)) {
            report.outcome = KillOutcome::Partial;
        } else {
            report.outcome = KillOutcome::Failed;
        }
        return report;
    }

    bool anyInitialTarget = false;
    for (const auto& target : report.targets) {
        if (target.wasRunning) {
            anyInitialTarget = true;
            break;
        }
    }
    const bool allInitialTargetsTerminated =
        anyInitialTarget && !AnyOriginalSurvived(report.targets);
    const bool trappedWithoutRestart =
        paralyzeFirst
        && AllTrapsSet(report.targets)
        && !AnyRunningAfter(report.targets);

    if ((!paralyzeFirst && allInitialTargetsTerminated)
        || trappedWithoutRestart) {
        report.outcome = KillOutcome::Succeeded;
    } else if (AnyTerminated(report.targets)
               || (paralyzeFirst && AllTrapsSet(report.targets))) {
        report.outcome = KillOutcome::Partial;
    } else {
        report.outcome = KillOutcome::Failed;
    }
    return report;
}

// ---------------------------------------------------------------------------
// Driver lifecycle
// ---------------------------------------------------------------------------

bool InitializeDriver() {
    return KillEngine::Start();
}

// ---------------------------------------------------------------------------
// Kill worker thread
// ---------------------------------------------------------------------------

DWORD WINAPI KillOnceThreadProc(LPVOID lpParam) {
    WorkerGuard guard{};

    auto* request = static_cast<KillThreadParams*>(lpParam);
    if (!request || request->targets.empty()) {
        delete request;
        if (!g_shutdownPending.load()) UpdateStatusText(LoadStr(IDS_STATUS_READY));
        SetButtonsEnabled(true);
        return 0;
    }

    const Vector<WString> targets = std::move(request->targets);
    const bool paralyze = request->paralyze;
    const bool defenderSet = request->defenderSet;
    delete request;

    const KillOperationReport report = ExecuteKillTargets(
        std::span<const WString>(targets.data(), targets.size()), paralyze, defenderSet);

    for (const KillTargetReport& target : report.targets) {
        if (target.trapSet) {
            StoreLastAction(target.name, kActionParalyzed);
        } else if (target.terminated) {
            StoreLastAction(target.name, kActionKilled);
        } else if (defenderSet && target.runningAfter) {
            StoreLastAction(target.name, kActionNone);
        }

        if (target.terminated) {
            ConfigReader::RecordHistory(
                L"KILL",
                target.name + L".exe",
                ConfigReader::ReadProcessPath(target.name));
        }
    }

    if (!g_shutdownPending.load()) {
        if (report.outcome == KillOutcome::Failed
            && !report.pplReady
            && !report.terminatorReady) {
            UpdateStatusText(LoadStr(IDS_STATUS_DRIVER_ERR));
        } else {
            switch (report.outcome) {
            case KillOutcome::Succeeded:
                UpdateStatusText(LoadStr(IDS_STATUS_KILL_ALL_OK));
                break;
            case KillOutcome::Partial:
                UpdateStatusText(LoadStr(IDS_STATUS_KILL_PARTIAL));
                break;
            case KillOutcome::AlreadyStopped:
                UpdateStatusText(LoadStr(
                    defenderSet
                        ? IDS_STATUS_KILL_ALL_OK
                        : IDS_STATUS_ALREADY_DEAD));
                break;
            case KillOutcome::Failed:
                UpdateStatusText(LoadStr(IDS_STATUS_KILL_FAILED));
                break;
            }
        }
        PostMessage(hInstMain, WMU_REFRESH_LIST, 0, 0);
    }
    SetButtonsEnabled(true);
    return 0;
}

// ---------------------------------------------------------------------------
// Restore worker thread
// ---------------------------------------------------------------------------

DWORD WINAPI RestoreThreadProc(LPVOID lpParam) {
    WorkerGuard guard{};

    auto* request = static_cast<RestoreThreadParams*>(lpParam);
    if (!request || request->targets.empty()) {
        delete request;
        UpdateStatusText(LoadStr(IDS_STATUS_READY));
        SetButtonsEnabled(true);
        return 0;
    }
    const Vector<WString> selected = std::move(request->targets);
    delete request;

    // Identify which selected processes are already running (result code 2 later).
    Vector<WString> exeNames;
    for (const auto& name : selected) exeNames.push_back(name + L".exe");

    Set<WString> initiallyRunning;
    for (const auto& proc : ProcessKiller::FindProcesses(exeNames))
        initiallyRunning.insert(NormalizeProcessKey(proc.name));

    Vector<WString> pendingNames;
    for (const auto& name : selected) {
        if (initiallyRunning.contains(NormalizeProcessKey(name))) continue;
        pendingNames.push_back(name);
        StoreLastAction(name, kActionRestoring);
    }

    if (!pendingNames.empty()) {
        if (!g_shutdownPending.load()) PostMessage(hInstMain, WMU_REFRESH_LIST, 0, 0);

        // --- Unparalyze Phase (BEFORE Restore) ---
        Vector<WString> toUnparalyze;
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
        Vector<WString> pendingExe;
        for (const auto& name : pendingNames) pendingExe.push_back(name + L".exe");

        Vector<int> results;
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
    MsgWaitForMultipleObjects(0, nullptr, FALSE, 0, QS_ALLINPUT);
    return 0;
}

// ---------------------------------------------------------------------------
// GUI action triggers  (called from WM_COMMAND on the UI thread)
// ---------------------------------------------------------------------------

namespace {

bool RequireElevation() {
    if (g_shutdownPending.load()) return false;
    if (!IsRunningAsAdmin()) {
        MessageBoxW(nullptr, LoadStr(IDS_ERR_NEED_ADMIN).c_str(),
                    LoadStr(IDS_ERR_CAPTION).c_str(), MB_ICONERROR);
        return false;
    }
    return true;
}

void StartKillWorker(Vector<WString> targets, bool paralyze,
                     bool defenderSet, UINT statusMessage) {
    if (targets.empty()) {
        UpdateStatusText(LoadStr(IDS_STATUS_READY));
        return;
    }

    UpdateStatusText(LoadStr(statusMessage));
    SetButtonsEnabled(false);

    auto* params = new KillThreadParams();
    params->targets = std::move(targets);
    params->paralyze = paralyze;
    params->defenderSet = defenderSet;

    g_activeWorkers.fetch_add(1);
    HANDLE hThread = CreateThread(
        nullptr, 0, KillOnceThreadProc, params, 0, nullptr);
    if (hThread) {
        CloseHandle(hThread);
    } else {
        delete params;
        SetButtonsEnabled(true);
        NotifyWorkerDone();
    }
}

} // namespace

void KillOnceAction() {
    if (!RequireElevation()) return;

    StartKillWorker(
        GetSelectedProcesses(),
        SendMessageW(hParalyzeCheck, BM_GETCHECK, 0, 0) == BST_CHECKED,
        false,
        IDS_STATUS_KILLING);
}

void KillDefenderAction() {
    if (!RequireElevation()) return;

    StartKillWorker(
        DefenderTargetList(),
        true,
        true,
        IDS_STATUS_KILLING_DEFENDER);
}

void RestoreAction() {
    if (!RequireElevation()) return;

    auto targets = GetSelectedProcesses();
    if (targets.empty()) targets = DefenderTargetList();

    UpdateStatusText(LoadStr(IDS_STATUS_RESTORING_SVC));
    SetButtonsEnabled(false);

    auto* params = new RestoreThreadParams();
    params->targets = std::move(targets);

    g_activeWorkers.fetch_add(1);
    HANDLE hThread = CreateThread(
        nullptr, 0, RestoreThreadProc, params, 0, nullptr);
    if (hThread) {
        CloseHandle(hThread);
    } else {
        delete params;
        SetButtonsEnabled(true);
        NotifyWorkerDone();
    }
}
