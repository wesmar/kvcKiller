#include "DriverPayload.h"
#include "GenIconSize.h"
#include "Resource.h"

#include <fdi.h>

#include <algorithm>
#include <cstring>

#pragma comment(lib, "cabinet.lib")

namespace {

constexpr int kMaxInputHandles = 8;
constexpr int kMaxPayloads = 8;
constexpr INT_PTR kOutputHandleBase = 0x100;

struct InputHandle {
    bool active = false;
    size_t offset = 0;
};

struct OutputHandle {
    bool active = false;
    bool complete = false;
    std::string name;
    std::vector<BYTE> data;
    size_t position = 0;
    size_t logicalSize = 0;
};

struct CabExtractionState {
    const BYTE* cabData = nullptr;
    size_t cabSize = 0;
    InputHandle inputs[kMaxInputHandles];
    OutputHandle outputs[kMaxPayloads];
};

thread_local CabExtractionState* g_state = nullptr;

bool IsInputHandle(INT_PTR handle) {
    return handle >= 1 && handle <= kMaxInputHandles;
}

bool IsOutputHandle(INT_PTR handle) {
    const INT_PTR index = handle - kOutputHandleBase;
    return index >= 0 && index < kMaxPayloads
        && g_state && g_state->outputs[index].active;
}

std::string BaseName(const char* path) {
    if (!path) return {};
    const char* name = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') name = p + 1;
    }
    return name;
}

bool EnsureCapacity(OutputHandle& output, size_t required) {
    if (required <= output.data.size()) return true;

    size_t capacity = output.data.size();
    if (capacity == 0) capacity = 64 * 1024;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) return false;
        capacity *= 2;
    }

    try {
        output.data.resize(capacity);
        return true;
    } catch (...) {
        return false;
    }
}

LPVOID DIAMONDAPI FdiAlloc(ULONG size) {
    return HeapAlloc(GetProcessHeap(), 0, size);
}

void DIAMONDAPI FdiFree(LPVOID memory) {
    if (memory) HeapFree(GetProcessHeap(), 0, memory);
}

INT_PTR DIAMONDAPI FdiOpen(char*, int, int) {
    if (!g_state) return -1;
    for (int i = 0; i < kMaxInputHandles; ++i) {
        if (!g_state->inputs[i].active) {
            g_state->inputs[i].active = true;
            g_state->inputs[i].offset = 0;
            return i + 1;
        }
    }
    return -1;
}

UINT DIAMONDAPI FdiRead(INT_PTR handle, void* destination, UINT requested) {
    if (!g_state || !IsInputHandle(handle)) return 0;

    InputHandle& input = g_state->inputs[handle - 1];
    if (!input.active) return 0;

    const size_t available = input.offset < g_state->cabSize
        ? g_state->cabSize - input.offset
        : 0;
    const size_t count = (std::min)(available, static_cast<size_t>(requested));
    if (count == 0) return 0;

    std::memcpy(destination, g_state->cabData + input.offset, count);
    input.offset += count;
    return static_cast<UINT>(count);
}

UINT DIAMONDAPI FdiWrite(INT_PTR handle, void* source, UINT count) {
    if (!g_state || !IsOutputHandle(handle) || !source) return 0;

    OutputHandle& output = g_state->outputs[handle - kOutputHandleBase];
    const size_t required = output.position + static_cast<size_t>(count);
    if (required < output.position || !EnsureCapacity(output, required)) return 0;

    std::memcpy(output.data.data() + output.position, source, count);
    output.position = required;
    if (required > output.logicalSize) output.logicalSize = required;
    return count;
}

int DIAMONDAPI FdiClose(INT_PTR handle) {
    if (g_state && IsInputHandle(handle)) {
        g_state->inputs[handle - 1].active = false;
        g_state->inputs[handle - 1].offset = 0;
    }
    return 0;
}

LONG DIAMONDAPI FdiSeek(INT_PTR handle, LONG distance, int origin) {
    if (!g_state) return -1;

    if (IsInputHandle(handle)) {
        InputHandle& input = g_state->inputs[handle - 1];
        if (!input.active) return -1;

        int64_t position = 0;
        if (origin == SEEK_SET) position = distance;
        else if (origin == SEEK_CUR) position = static_cast<int64_t>(input.offset) + distance;
        else if (origin == SEEK_END) position = static_cast<int64_t>(g_state->cabSize) + distance;
        else return -1;

        if (position < 0) return -1;
        input.offset = (std::min)(static_cast<size_t>(position), g_state->cabSize);
        return static_cast<LONG>(input.offset);
    }

    if (IsOutputHandle(handle)) {
        OutputHandle& output = g_state->outputs[handle - kOutputHandleBase];
        int64_t position = 0;
        if (origin == SEEK_SET) position = distance;
        else if (origin == SEEK_CUR) position = static_cast<int64_t>(output.position) + distance;
        else if (origin == SEEK_END) position = static_cast<int64_t>(output.logicalSize) + distance;
        else return -1;

        if (position < 0) return -1;
        output.position = static_cast<size_t>(position);
        if (!EnsureCapacity(output, output.position)) return -1;
        return static_cast<LONG>(output.position);
    }

    return -1;
}

INT_PTR DIAMONDAPI FdiNotify(FDINOTIFICATIONTYPE type, PFDINOTIFICATION notification) {
    if (!g_state || !notification) return 0;

    if (type == fdintCOPY_FILE) {
        const std::string name = BaseName(notification->psz1);
        if (name.empty()) return -1;

        int index = -1;
        for (int i = 0; i < kMaxPayloads; ++i) {
            if (g_state->outputs[i].active && g_state->outputs[i].name == name) {
                index = i;
                break;
            }
            if (index < 0 && !g_state->outputs[i].active) index = i;
        }
        if (index < 0) return -1;

        OutputHandle& output = g_state->outputs[index];
        output.active = true;
        output.complete = false;
        output.name = name;
        output.data.clear();
        output.position = 0;
        output.logicalSize = 0;

        const LONG expected = notification->cb;
        if (expected > 0
            && !EnsureCapacity(output, static_cast<size_t>(expected))) {
            output.active = false;
            return -1;
        }

        return kOutputHandleBase + index;
    }

    if (type == fdintCLOSE_FILE_INFO) {
        if (!IsOutputHandle(notification->hf)) return FALSE;
        OutputHandle& output = g_state->outputs[notification->hf - kOutputHandleBase];
        output.data.resize(output.logicalSize);
        output.position = output.logicalSize;
        output.complete = true;
        return TRUE;
    }

    return 0;
}

} // namespace

bool ExtractDriverPayloads(std::vector<DriverPayloadFile>& files) {
    files.clear();

    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_DRIVER_PAYLOAD), RT_RCDATA);
    if (!resource) return false;

    const DWORD resourceSize = SizeofResource(nullptr, resource);
    if (resourceSize <= ICON_HEADER_SIZE + 4) return false;

    HGLOBAL loaded = LoadResource(nullptr, resource);
    if (!loaded) return false;

    const BYTE* raw = static_cast<const BYTE*>(LockResource(loaded));
    if (!raw) return false;

    const BYTE* cab = raw + ICON_HEADER_SIZE;
    const size_t cabSize = resourceSize - ICON_HEADER_SIZE;
    if (cabSize < 4 || std::memcmp(cab, "MSCF", 4) != 0) return false;

    CabExtractionState state;
    state.cabData = cab;
    state.cabSize = cabSize;
    g_state = &state;

    ERF errorInfo{};
    HFDI fdi = FDICreate(FdiAlloc, FdiFree, FdiOpen, FdiRead, FdiWrite,
                         FdiClose, FdiSeek, cpuUNKNOWN, &errorInfo);
    if (!fdi) {
        g_state = nullptr;
        return false;
    }

    const BOOL extracted = FDICopy(
        fdi,
        const_cast<char*>("kvckiller.cab"),
        const_cast<char*>(""),
        0,
        FdiNotify,
        nullptr,
        nullptr
    );
    FDIDestroy(fdi);
    g_state = nullptr;

    if (!extracted) {
        for (OutputHandle& output : state.outputs) {
            output.data.clear();
        }
        return false;
    }

    for (OutputHandle& output : state.outputs) {
        if (!output.active || !output.complete || output.data.empty()) continue;
        files.push_back({ output.name, std::move(output.data) });
    }

    return !files.empty();
}
