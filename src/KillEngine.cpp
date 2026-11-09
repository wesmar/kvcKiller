#include "KillEngine.h"
#include "DriverController.h"
#include "ProcessKiller.h"

#include <chrono>
#include <thread>

HANDLE KillEngine::g_pplDevice = INVALID_HANDLE_VALUE;
HANDLE KillEngine::g_terminatorDevice = INVALID_HANDLE_VALUE;
bool KillEngine::g_pplReady = false;
bool KillEngine::g_terminatorReady = false;

bool KillEngine::Start() {
    Stop();

    g_pplDevice = DriverController::LoadDriver(DriverRole::PplUnprotector);
    if (g_pplDevice != INVALID_HANDLE_VALUE) {
        g_pplReady = PplUnprotector::Initialize(g_pplDevice);
    }

    g_terminatorDevice = DriverController::LoadDriver(
        DriverRole::ProcessTerminator
    );
    if (g_terminatorDevice != INVALID_HANDLE_VALUE) {
        g_terminatorReady = ProcessKiller::InitializeTerminator(
            g_terminatorDevice
        );
    }

    return g_pplReady || g_terminatorReady;
}

void KillEngine::Stop() {
    g_pplReady = false;
    g_terminatorReady = false;
    g_pplDevice = INVALID_HANDLE_VALUE;
    g_terminatorDevice = INVALID_HANDLE_VALUE;
    DriverController::CleanupAll();
}

bool KillEngine::HasPplUnprotector() {
    return g_pplReady;
}

bool KillEngine::HasProcessTerminator() {
    return g_terminatorReady;
}

bool KillEngine::LowerProtection(const std::vector<DWORD>& pids,
                                 std::vector<PplTargetResult>& results) {
    if (!g_pplReady) {
        results.clear();
        return false;
    }
    return PplUnprotector::LowerProtection(pids, results);
}

KillAttemptResult KillEngine::Kill(DWORD pid) {
    KillAttemptResult result{};
    if (pid == 0 || !ProcessKiller::IsProcessRunning(pid)) {
        result.terminated = true;
        return result;
    }

    if (g_terminatorReady) {
        result.requested = ProcessKiller::SendIOCTL(
            g_terminatorDevice,
            pid
        );
        if (!result.requested) {
            result.driverError = GetLastError();
        }
    } else {
        result.driverError = ERROR_DEVICE_NOT_CONNECTED;
    }

    if (result.requested) {
        for (int i = 0; i < 30; ++i) {
            if (!ProcessKiller::IsProcessRunning(pid)) {
                result.terminated = true;
                return result;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    if (!result.terminated) {
        result.usedUserModeFallback = true;
        HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (process) {
            result.terminated = TerminateProcess(process, 1) != FALSE;
            if (result.terminated) {
                WaitForSingleObject(process, 1500);
                result.terminated = !ProcessKiller::IsProcessRunning(pid);
            }
            CloseHandle(process);
        }
    }

    return result;
}
