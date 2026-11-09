#pragma once

#include "PplUnprotector.h"

#include <windows.h>

#include <vector>

struct KillAttemptResult {
    bool requested = false;
    bool terminated = false;
    bool usedUserModeFallback = false;
    DWORD driverError = ERROR_SUCCESS;
};

class KillEngine {
public:
    // Loads and validates both kernel components. The session remains usable
    // when only the termination driver is available, but PPL targets will then
    // remain protected and can only report a failed kill.
    static bool Start();
    static void Stop();

    static bool HasPplUnprotector();
    static bool HasProcessTerminator();

    // Performs one tree walk for the complete target batch.
    static bool LowerProtection(const std::vector<DWORD>& pids,
                                std::vector<PplTargetResult>& results);

    // Sends the kernel termination request and verifies that the process
    // object is signalled. A user-mode fallback is attempted when the kernel
    // request is rejected or does not terminate the process within the wait
    // window.
    static KillAttemptResult Kill(DWORD pid);

private:
    static HANDLE g_pplDevice;
    static HANDLE g_terminatorDevice;
    static bool g_pplReady;
    static bool g_terminatorReady;
};
