// CLIHandler.cpp
// © WESMAR Marek Wesołowski
//
// Headless command-line interface for KvcKiller.
// WinMain delegates here when argc > 1; this module never touches any GUI handles.
//
// Supported commands:
//   -help                    Print usage and exit.
//   -list                    Enumerate config targets and show running status.
//   -kill <name> [-paralyze] Kill one process, optionally applying IFEO first.
//   -kill-defender           Kill/trap the fixed Defender process set.
//   -restore <name>          Reverse a prior kill / paralyze.
//   -restore-defender        Restore the fixed Defender process set.

#include "CLIHandler.h"
#include "Utils.h"
#include "KillEngine.h"
#include "ProcessKiller.h"
#include "ConfigReader.h"
#include "ProcessOperations.h"
#include "Resource.h"
#include "Console.h"
#include "StrUtils.h"
#include "KvcContainers.h"

#include <array>
#include <span>
#include <string_view>

// ---------------------------------------------------------------------------
// Console setup
// ---------------------------------------------------------------------------

void SetupConsole() {
    Console::Setup();
}

void SendEnterToConsole() {
    Console::SendEnter();
}

// ---------------------------------------------------------------------------
// Help
// ---------------------------------------------------------------------------

namespace {

void PrintUsageRow(std::wstring_view syntax, std::wstring_view description) {
    constexpr size_t kDescriptionColumn = 48;
    WString row;
    row.reserve(kDescriptionColumn + description.size() + 2);
    row.append(L"  ");
    row.append(syntax);
    if (row.size() < kDescriptionColumn) {
        row.append(kDescriptionColumn - row.size(), L' ');
    } else {
        row.push_back(L' ');
    }
    row.append(description);
    Console::PrintLine(row.c_str());
}

int ExitCodeForKillOutcome(KillOutcome outcome) {
    switch (outcome) {
    case KillOutcome::Succeeded:
    case KillOutcome::AlreadyStopped:
        return 0;
    case KillOutcome::Partial:
        return 2;
    case KillOutcome::Failed:
        return 1;
    }
    return 1;
}

void PrintKillReport(const KillOperationReport& report) {
    Console::PrintLineFormat(L"[*] PPL unprotector: %ls",
            report.pplReady ? L"ready" : L"unavailable");
    Console::PrintLineFormat(L"[*] Process terminator: %ls",
            report.terminatorReady ? L"ready" : L"unavailable");

    for (size_t i = 0; i < report.targets.size(); ++i) {
        const auto& target = report.targets[i];
        WString exeName = target.name;
        exeName += L".exe";
        if (target.terminated) {
            Console::PrintLineFormat(L"[+] %-30ls (PID %lu) terminated",
                    exeName.c_str(), target.pid);
        } else if (!target.wasRunning && !target.runningAfter) {
            Console::PrintLineFormat(L"[*] %-30ls not running", exeName.c_str());
        }

        if (target.restartedAfter) {
            Console::PrintLineFormat(L"[*] %-30ls restarted with a new PID after termination",
                    exeName.c_str());
        } else if (target.originalSurvived) {
            Console::PrintLineFormat(L"[*] %-30ls still running", exeName.c_str());
        }

        if (target.trapSet) {
            Console::PrintLineFormat(L"[+] %-30ls IFEO trap active", exeName.c_str());
        }
    }

    if (report.defenderSet) {
        Console::PrintLineFormat(L"[%c] WinDefend service: %ls",
                report.winDefendStopped ? L'+' : L'!',
                report.winDefendStopped ? L"stopped" : L"not stopped");
        Console::PrintLineFormat(L"[%c] SecurityHealthService: %ls",
                report.securityHealthStopped ? L'+' : L'*',
                report.securityHealthStopped ? L"stopped" : L"still active");
    }

    switch (report.outcome) {
    case KillOutcome::Succeeded:
        Console::PrintLine(L"[+] KILL COMPLETE - ALL REQUIRED CONTROLS CONFIRMED");
        break;
    case KillOutcome::AlreadyStopped:
        Console::PrintLine(L"[*] Target was already stopped.");
        break;
    case KillOutcome::Partial:
        Console::PrintLine(L"[!] KILL PARTIAL - one or more required controls were not confirmed");
        break;
    case KillOutcome::Failed:
        Console::PrintLine(L"[-] KILL FAILED - no required control was confirmed");
        break;
    }
}

bool RestoreTargets(std::span<const WString> names) {
    Vector<WString> bareNames;
    Vector<WString> exeNames;
    bareNames.reserve(names.size());
    exeNames.reserve(names.size());
    for (size_t i = 0; i < names.size(); ++i) {
        bareNames.push_back(StripExeExtension(names[i]));
        WString exe = bareNames.back();
        exe += L".exe";
        exeNames.push_back(exe);
    }

    Vector<WString> trapped;
    for (size_t i = 0; i < bareNames.size(); ++i) {
        if (IsProcessParalyzed(bareNames[i])) trapped.push_back(bareNames[i]);
    }
    if (!trapped.empty()) {
        const int released = UnparalyzeProcessesBatch(trapped);
        Console::PrintLineFormat(L"[*] IFEO traps removed: %d/%u",
                released, static_cast<unsigned int>(trapped.size()));
    }

    Vector<int> results;
    ProcessKiller::RestoreProcesses(exeNames, results);

    bool allReady = true;
    for (size_t i = 0; i < exeNames.size(); ++i) {
        if (results[i] == 1 || results[i] == 2) {
            ConfigReader::RecordHistory(
                L"RESTORE",
                exeNames[i],
                ConfigReader::ReadProcessPath(bareNames[i]));
            Console::PrintLineFormat(L"[+] %-30ls %ls",
                    exeNames[i].c_str(),
                    results[i] == 2 ? L"already running" : L"start requested");
        } else {
            allReady = false;
            Console::PrintLineFormat(L"[!] %-30ls could not be restored",
                    exeNames[i].c_str());
        }
    }
    return allReady;
}

} // namespace

void PrintHelp() {
    Console::PrintLineFormat(L"KvcKiller - %ls", LoadStr(IDS_APP_SUBTITLE).c_str());
    Console::PrintLine(L"WESMAR Marek Weso\x0142owski\r\n");
    Console::PrintLine(L"Usage:");
    PrintUsageRow(L"KvcKiller.exe -help",
                  L"Show this help message (.exe is optional)");
    PrintUsageRow(L"KvcKiller.exe -check-drivers",
                  L"Load and verify both kernel components");
    PrintUsageRow(L"KvcKiller.exe -list",
                  L"List configured targets and their status");
    PrintUsageRow(L"KvcKiller.exe -kill <name> [-paralyze]",
                  L"Kill one process; optionally apply IFEO first");
    PrintUsageRow(L"KvcKiller.exe -kill-defender",
                  L"Trap and kill the fixed Defender process set");
    PrintUsageRow(L"KvcKiller.exe -restore <name>",
                  L"Restore one process and remove its IFEO trap");
    PrintUsageRow(L"KvcKiller.exe -restore-defender",
                  L"Restore the fixed Defender process set");
    Console::PrintLine();
    Console::PrintLine(L"Examples:");
    PrintUsageRow(L"KvcKiller.exe -list", L"");
    PrintUsageRow(L"KvcKiller.exe -kill MsMpEng",
                  L"Terminate MsMpEng without a restart trap");
    PrintUsageRow(L"KvcKiller.exe -kill MsMpEng -paralyze",
                  L"Trap first, then terminate");
    PrintUsageRow(L"KvcKiller.exe -kill-defender",
                  L"Use the same three-process action as the GUI");
    PrintUsageRow(L"KvcKiller.exe -restore-defender",
                  L"Reverse the three-process action");
}

// ---------------------------------------------------------------------------
// CLI dispatcher
// ---------------------------------------------------------------------------

// Entry point called from WinMain when argc > 1.
// Returns an exit code suitable for use as the process exit code.
//
// finishCli is a local lambda that handles all cleanup paths:
//   - optionally closes the driver handle (frees the IOCTL channel)
//   - injects the synthetic Enter so the parent shell redraws its prompt
//   - releases the console handle
int RunCLI(int argc, LPWSTR* argv) {
    MsgWaitForMultipleObjects(0, nullptr, FALSE, 0, QS_ALLINPUT);
    SetupConsole();

    // Single cleanup point for all return paths.
    // closeDriver=true releases both kernel components and their artifacts.
    auto finishCli = [](int code, bool closeDriver = false) -> int {
        if (closeDriver) KillEngine::Stop();
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 0, QS_ALLINPUT);
        Console::SendEnter();
        if (Console::IsConsoleAttached()) {
            FreeConsole();
        }
        return code;
    };

    const WString cmd = argv[1];

    // -help / --help / /?  — does not require admin or driver
    if (cmd == L"-help" || cmd == L"--help" || cmd == L"/?") {
        PrintHelp();
        return finishCli(0);
    }

    // All other commands require elevation.
    if (!IsRunningAsAdmin()) {
        Console::PrintLine(L"[!] ERROR: Please run as Administrator!");
        return finishCli(1);
    }

    if (cmd == L"-check-drivers") {
        const bool started = KillEngine::Start();
        bool pplReady = KillEngine::HasPplUnprotector();
        const bool terminatorReady = KillEngine::HasProcessTerminator();

        if (pplReady) {
            PplTargetResult probe{};
            pplReady = PplUnprotector::Probe(GetCurrentProcessId(), probe);
        }

        Console::PrintLineFormat(L"[*] PPL unprotector: %ls", pplReady ? L"ready" : L"unavailable");
        Console::PrintLineFormat(L"[*] Process terminator: %ls", terminatorReady ? L"ready" : L"unavailable");

        if (!started) return finishCli(1, true);
        if (pplReady && terminatorReady) return finishCli(0, true);
        return finishCli(pplReady ? 2 : 3, true);
    }

    // -list: enumerate config targets and show running/not-running status
    if (cmd == L"-list") {
        Console::PrintLine(L"[*] Scanning for targets...\r\n");

        auto processNames = ConfigReader::ReadProcessList();
        Vector<WString> exeNames;
        exeNames.reserve(processNames.size());
        for (size_t i = 0; i < processNames.size(); ++i) {
            WString exe = processNames[i];
            exe += L".exe";
            exeNames.push_back(exe);
        }

        auto running = ProcessKiller::FindProcesses(exeNames);

        Console::PrintLineFormat(L"%-30ls | %-10ls | %-10ls", L"Process Name", L"PID", L"Status");
        Console::PrintLine(L"-------------------------------+------------+-----------");

        for (size_t i = 0; i < processNames.size(); ++i) {
            WString exeName = processNames[i];
            exeName += L".exe";

            // Case-insensitive search in the running list.
            const ProcessInfo* found = nullptr;
            for (size_t j = 0; j < running.size(); ++j) {
                if (StrUtils::StrEqualNoCase(running[j].name.c_str(), exeName.c_str())) {
                    found = &running[j];
                    break;
                }
            }

            if (found != nullptr)
                Console::PrintLineFormat(L"%-30ls | %-10lu | RUNNING",    exeName.c_str(), found->pid);
            else
                Console::PrintLineFormat(L"%-30ls | %-10ls | NOT RUNNING", exeName.c_str(), L"-");
        }

        return finishCli(0);
    }

    if (cmd == L"-kill-defender") {
        Console::PrintLine(L"[*] Running the fixed Defender set...");
        const Vector<WString> targets = DefenderTargetList();
        const KillOperationReport report = ExecuteKillTargets(
            targets.span(), true, true);
        PrintKillReport(report);
        return finishCli(ExitCodeForKillOutcome(report.outcome));
    }

    // -kill <name> [-paralyze] or -kill -paralyze <name>
    if (cmd == L"-kill") {
        if (argc < 3) {
            Console::PrintLine(L"[!] ERROR: Missing process name.\r\n"
                               L"    Usage: KvcKiller.exe -kill <name> [-paralyze]");
            return finishCli(1);
        }

        bool doParalyze = false;
        WString targetStr;
        for (int i = 2; i < argc; ++i) {
            if (StrUtils::StrEqualNoCase(argv[i], L"-paralyze")) {
                doParalyze = true;
            } else if (targetStr.empty()) {
                targetStr = argv[i];
            } else {
                Console::PrintLineFormat(L"[!] ERROR: Unexpected argument: %ls", argv[i]);
                return finishCli(1);
            }
        }
        if (targetStr.empty()) {
            Console::PrintLine(L"[!] ERROR: Missing process name.");
            return finishCli(1);
        }

        const WString targets[1] = {
            StripExeExtension(targetStr)
        };
        Console::PrintLineFormat(L"[*] Target: %ls.exe", targets[0].c_str());
        const KillOperationReport report = ExecuteKillTargets(
            std::span<const WString>(targets, 1), doParalyze, false);
        PrintKillReport(report);
        return finishCli(ExitCodeForKillOutcome(report.outcome));
    }

    if (cmd == L"-restore-defender") {
        Console::PrintLine(L"[*] Restoring the fixed Defender set...");
        const Vector<WString> targets = DefenderTargetList();
        return finishCli(RestoreTargets(targets.span()) ? 0 : 1);
    }

    // -restore <name>
    if (cmd == L"-restore") {
        if (argc < 3) {
            Console::PrintLine(L"[!] ERROR: Missing process name.\r\n"
                               L"    Usage: KvcKiller.exe -restore <name>");
            return finishCli(1);
        }

        const WString targets[1] = {
            StripExeExtension(argv[2])
        };
        return finishCli(RestoreTargets(std::span<const WString>(targets, 1)) ? 0 : 1);
    }

    // Unknown command — print help so the user knows what went wrong.
    Console::PrintLineFormat(L"[!] Unknown command: %ls", cmd.c_str());
    PrintHelp();
    return finishCli(1, true);
}
