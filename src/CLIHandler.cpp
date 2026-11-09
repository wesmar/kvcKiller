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

#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include <io.h>
#include <fcntl.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Console setup
// ---------------------------------------------------------------------------

// Attaches to the parent process console (the cmd.exe or PowerShell window that
// launched us) and re-opens stdin/stdout/stderr against it.
//
// AttachConsole is used instead of AllocConsole because we are a GUI subsystem
// binary (/SUBSYSTEM:WINDOWS).  AttachConsole lets us print into the *existing*
// console session rather than popping a new window — but it does nothing if the
// parent has no console (e.g., double-clicked from Explorer).
//
// _setmode(_O_U16TEXT) must be set AFTER freopen_s so the CRT knows the
// underlying handle is Unicode-capable; without it, wprintf to a Unicode console
// silently drops non-ASCII characters.
void SetupConsole() {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* fp;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        freopen_s(&fp, "CONIN$",  "r", stdin);
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
    }

    // Preserve inherited redirected handles when no console is available.
    // This keeps CLI diagnostics usable from CI and script launchers.
    if (GetStdHandle(STD_OUTPUT_HANDLE) != INVALID_HANDLE_VALUE)
        _setmode(_fileno(stdout), _O_U16TEXT);
    if (GetStdHandle(STD_ERROR_HANDLE) != INVALID_HANDLE_VALUE)
        _setmode(_fileno(stderr), _O_U16TEXT);
    if (GetStdHandle(STD_INPUT_HANDLE) != INVALID_HANDLE_VALUE)
        _setmode(_fileno(stdin), _O_U16TEXT);
}

// ---------------------------------------------------------------------------
// Synthetic Enter — fixes the "missing prompt redraw" issue with AttachConsole
// ---------------------------------------------------------------------------

// When a GUI-subsystem process attaches to a console and then exits, the parent
// shell does not redraw its prompt because it never got a signal that our
// process finished (it wasn't waiting for us in the first place).  Injecting a
// synthetic Enter key-press into the console input queue forces cmd.exe /
// PowerShell to re-draw the prompt line after we return.
//
// Both a key-down and key-up event are injected; some shells ignore one or the other.
void SendEnterToConsole() {
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    if (hStdin == INVALID_HANDLE_VALUE) return;

    FlushConsoleInputBuffer(hStdin);

    INPUT_RECORD ir[2] = {};

    // Key-down event for VK_RETURN.
    ir[0].EventType                        = KEY_EVENT;
    ir[0].Event.KeyEvent.bKeyDown          = TRUE;
    ir[0].Event.KeyEvent.wRepeatCount      = 1;
    ir[0].Event.KeyEvent.wVirtualKeyCode   = VK_RETURN;
    ir[0].Event.KeyEvent.wVirtualScanCode  =
        static_cast<WORD>(MapVirtualKeyW(VK_RETURN, MAPVK_VK_TO_VSC));
    ir[0].Event.KeyEvent.uChar.UnicodeChar = L'\r';
    ir[0].Event.KeyEvent.dwControlKeyState = 0;

    // Matching key-up event (copy of key-down with bKeyDown flipped).
    ir[1]                         = ir[0];
    ir[1].Event.KeyEvent.bKeyDown = FALSE;

    DWORD written = 0;
    WriteConsoleInputW(hStdin, ir, 2, &written);
}

// ---------------------------------------------------------------------------
// Help
// ---------------------------------------------------------------------------

namespace {

void PrintUsageRow(std::wstring_view syntax, std::wstring_view description) {
    constexpr size_t kDescriptionColumn = 48;
    std::wstring row;
    row.reserve(kDescriptionColumn + description.size() + 2);
    row.append(L"  ");
    row.append(syntax);
    if (row.size() < kDescriptionColumn) {
        row.append(kDescriptionColumn - row.size(), L' ');
    } else {
        row.push_back(L' ');
    }
    row.append(description);
    wprintf(L"%ls\n", row.c_str());
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
    wprintf(L"[*] PPL unprotector: %ls\n",
            report.pplReady ? L"ready" : L"unavailable");
    wprintf(L"[*] Process terminator: %ls\n",
            report.terminatorReady ? L"ready" : L"unavailable");

    for (const KillTargetReport& target : report.targets) {
        const std::wstring exeName = target.name + L".exe";
        if (target.terminated) {
            wprintf(L"[+] %-30ls (PID %lu) terminated\n",
                    exeName.c_str(), target.pid);
        } else if (!target.wasRunning && !target.runningAfter) {
            wprintf(L"[*] %-30ls not running\n", exeName.c_str());
        }

        if (target.restartedAfter) {
            wprintf(L"[*] %-30ls restarted with a new PID after termination\n",
                    exeName.c_str());
        } else if (target.originalSurvived) {
            wprintf(L"[*] %-30ls still running\n", exeName.c_str());
        }

        if (target.trapSet) {
            wprintf(L"[+] %-30ls IFEO trap active\n", exeName.c_str());
        }
    }

    if (report.defenderSet) {
        wprintf(L"[%c] WinDefend service: %ls\n",
                report.winDefendStopped ? L'+' : L'!',
                report.winDefendStopped ? L"stopped" : L"not stopped");
        wprintf(L"[%c] SecurityHealthService: %ls\n",
                report.securityHealthStopped ? L'+' : L'*',
                report.securityHealthStopped ? L"stopped" : L"still active");
    }

    switch (report.outcome) {
    case KillOutcome::Succeeded:
        wprintf(L"[+] KILL COMPLETE - ALL REQUIRED CONTROLS CONFIRMED\n");
        break;
    case KillOutcome::AlreadyStopped:
        wprintf(L"[*] Target was already stopped.\n");
        break;
    case KillOutcome::Partial:
        wprintf(L"[!] KILL PARTIAL - one or more required controls were not confirmed\n");
        break;
    case KillOutcome::Failed:
        wprintf(L"[-] KILL FAILED - no required control was confirmed\n");
        break;
    }
}

bool RestoreTargets(std::span<const std::wstring> names) {
    std::vector<std::wstring> bareNames;
    std::vector<std::wstring> exeNames;
    bareNames.reserve(names.size());
    exeNames.reserve(names.size());
    for (const std::wstring& name : names) {
        bareNames.push_back(StripExeExtension(name));
        exeNames.push_back(bareNames.back() + L".exe");
    }

    std::vector<std::wstring> trapped;
    for (const std::wstring& name : bareNames) {
        if (IsProcessParalyzed(name)) trapped.push_back(name);
    }
    if (!trapped.empty()) {
        const int released = UnparalyzeProcessesBatch(trapped);
        wprintf(L"[*] IFEO traps removed: %d/%zu\n",
                released, trapped.size());
    }

    std::vector<int> results;
    ProcessKiller::RestoreProcesses(exeNames, results);

    bool allReady = true;
    for (size_t i = 0; i < exeNames.size(); ++i) {
        if (results[i] == 1 || results[i] == 2) {
            ConfigReader::RecordHistory(
                L"RESTORE",
                exeNames[i],
                ConfigReader::ReadProcessPath(bareNames[i]));
            wprintf(L"[+] %-30ls %ls\n",
                    exeNames[i].c_str(),
                    results[i] == 2 ? L"already running" : L"start requested");
        } else {
            allReady = false;
            wprintf(L"[!] %-30ls could not be restored\n",
                    exeNames[i].c_str());
        }
    }
    return allReady;
}

} // namespace

void PrintHelp() {
    wprintf(L"\nKvcKiller - %ls\n", LoadStr(IDS_APP_SUBTITLE).c_str());
    wprintf(L"WESMAR Marek Wesołowski\n\n");
    wprintf(L"Usage:\n");
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
    wprintf(L"\n");
    wprintf(L"Examples:\n");
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
//   - flushes stdout/stderr so buffered output is not lost
//   - optionally closes the driver handle (frees the IOCTL channel)
//   - injects the synthetic Enter so the parent shell redraws its prompt
//   - releases the console handle
int RunCLI(int argc, LPWSTR* argv) {
    SetupConsole();

    // Single cleanup point for all return paths.
    // closeDriver=true releases both kernel components and their artifacts.
    auto finishCli = [](int code, bool closeDriver = false) -> int {
        if (closeDriver) KillEngine::Stop();
        fflush(stdout);
        fflush(stderr);
        SendEnterToConsole();
        FreeConsole();
        return code;
    };

    const std::wstring cmd = argv[1];

    // -help / --help / /?  — does not require admin or driver
    if (cmd == L"-help" || cmd == L"--help" || cmd == L"/?") {
        PrintHelp();
        return finishCli(0);
    }

    // All other commands require elevation.
    if (!IsRunningAsAdmin()) {
        wprintf(L"[!] ERROR: Please run as Administrator!\n");
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

        wprintf(L"[*] PPL unprotector: %ls\n", pplReady ? L"ready" : L"unavailable");
        wprintf(L"[*] Process terminator: %ls\n", terminatorReady ? L"ready" : L"unavailable");

        if (!started) return finishCli(1, true);
        if (pplReady && terminatorReady) return finishCli(0, true);
        return finishCli(pplReady ? 2 : 3, true);
    }

    // -list: enumerate config targets and show running/not-running status
    if (cmd == L"-list") {
        wprintf(L"[*] Scanning for targets...\n\n");

        auto processNames = ConfigReader::ReadProcessList();
        std::vector<std::wstring> exeNames;
        for (const auto& name : processNames) exeNames.push_back(name + L".exe");

        auto running = ProcessKiller::FindProcesses(exeNames);

        wprintf(L"%-30s | %-10s | %-10s\n", L"Process Name", L"PID", L"Status");
        wprintf(L"-------------------------------+------------+-----------\n");

        for (const auto& name : processNames) {
            const std::wstring exeName = name + L".exe";
            // Case-insensitive search in the running list.
            const auto it = std::find_if(running.begin(), running.end(),
                [&exeName](const ProcessInfo& p) {
                    return _wcsicmp(p.name.c_str(), exeName.c_str()) == 0;
                });

            if (it != running.end())
                wprintf(L"%-30s | %-10lu | RUNNING\n",     exeName.c_str(), it->pid);
            else
                wprintf(L"%-30s | %-10s | NOT RUNNING\n",  exeName.c_str(), L"-");
        }

        return finishCli(0);
    }

    if (cmd == L"-kill-defender") {
        wprintf(L"[*] Running the fixed Defender set...\n");
        const std::vector<std::wstring> targets = DefenderTargetList();
        const KillOperationReport report = ExecuteKillTargets(
            targets, true, true);
        PrintKillReport(report);
        return finishCli(ExitCodeForKillOutcome(report.outcome));
    }

    // -kill <name> [-paralyze] or -kill -paralyze <name>
    if (cmd == L"-kill") {
        if (argc < 3) {
            wprintf(L"[!] ERROR: Missing process name.\n"
                    L"    Usage: KvcKiller.exe -kill <name> [-paralyze]\n");
            return finishCli(1);
        }

        bool doParalyze = false;
        std::wstring targetStr;
        for (int i = 2; i < argc; ++i) {
            if (_wcsicmp(argv[i], L"-paralyze") == 0) {
                doParalyze = true;
            } else if (targetStr.empty()) {
                targetStr = argv[i];
            } else {
                wprintf(L"[!] ERROR: Unexpected argument: %ls\n", argv[i]);
                return finishCli(1);
            }
        }
        if (targetStr.empty()) {
            wprintf(L"[!] ERROR: Missing process name.\n");
            return finishCli(1);
        }

        const std::array<std::wstring, 1> targets{
            StripExeExtension(targetStr)
        };
        wprintf(L"[*] Target: %ls.exe\n", targets.front().c_str());
        const KillOperationReport report = ExecuteKillTargets(
            targets, doParalyze, false);
        PrintKillReport(report);
        return finishCli(ExitCodeForKillOutcome(report.outcome));
    }

    if (cmd == L"-restore-defender") {
        wprintf(L"[*] Restoring the fixed Defender set...\n");
        const std::vector<std::wstring> targets = DefenderTargetList();
        return finishCli(RestoreTargets(targets) ? 0 : 1);
    }

    // -restore <name>
    if (cmd == L"-restore") {
        if (argc < 3) {
            wprintf(L"[!] ERROR: Missing process name.\n"
                    L"    Usage: KvcKiller.exe -restore <name>\n");
            return finishCli(1);
        }

        const std::array<std::wstring, 1> targets{
            StripExeExtension(argv[2])
        };
        return finishCli(RestoreTargets(targets) ? 0 : 1);
    }

    // Unknown command — print help so the user knows what went wrong.
    wprintf(L"[!] Unknown command: %ls\n", cmd.c_str());
    PrintHelp();
    return finishCli(1, true);
}
