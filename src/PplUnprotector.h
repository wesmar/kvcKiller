#pragma once

#include <windows.h>
#include "KvcContainers.h"

struct PplTargetResult {
    DWORD pid = 0;
    bool found = false;
    bool lowered = false;
    BYTE protection = 0;
};

class PplUnprotector {
public:
    // Resolves the kernel and EPROCESS offsets once per kill session.
    static bool Initialize(HANDLE memoryDevice);

    // Performs a read-only EPROCESS walk and reports whether pid was found.
    static bool Probe(DWORD pid, PplTargetResult& result);

    // Lowers the EPROCESS protection field for the selected PIDs. The return
    // value reports a completed traversal; per-process outcomes are returned
    // in results and may still contain misses or write failures.
    static bool LowerProtection(const Vector<DWORD>& pids,
                                Vector<PplTargetResult>& results);

    static bool IsReady();
};
