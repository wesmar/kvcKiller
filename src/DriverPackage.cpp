#include "DriverPackage.h"
#include "Resource.h"
#include "RuntimeSupport.h"
#include "StrUtils.h"

#include <fdi.h>

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

namespace {

constexpr int kMaxInputHandles = 8;
constexpr int kMaxOutputs = 8;
constexpr INT_PTR kOutputHandleBase = 0x100;

struct InputHandle {
    bool active = false;
    size_t offset = 0;
};

struct OutputHandle {
    bool active = false;
    bool complete = false;
    AString name;
    Vector<BYTE> data;
    size_t position = 0;
    size_t logicalSize = 0;
};

struct CabExtractionState {
    const BYTE* cabData = nullptr;
    size_t cabSize = 0;
    InputHandle inputs[kMaxInputHandles];
    OutputHandle outputs[kMaxOutputs];
};

static CabExtractionState* g_state = nullptr;

bool IsInputHandle(INT_PTR handle) {
    return handle >= 1 && handle <= kMaxInputHandles;
}

bool IsOutputHandle(INT_PTR handle) {
    const INT_PTR index = handle - kOutputHandleBase;
    return index >= 0 && index < kMaxOutputs
        && g_state && g_state->outputs[index].active;
}

AString BaseName(const char* path) {
    if (!path) return AString();
    const char* name = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') name = p + 1;
    }
    return AString(name);
}

bool EnsureCapacity(OutputHandle& output, size_t required) {
    if (required <= output.data.capacity()) return true;

    size_t capacity = output.data.capacity();
    if (capacity == 0) capacity = 64 * 1024;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) return false;
        capacity *= 2;
    }

    return output.data.reserve(capacity);
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

    if (input.offset >= g_state->cabSize) return 0;
    const size_t available = g_state->cabSize - input.offset;
    const UINT toRead = (requested < available) ? requested : static_cast<UINT>(available);
    if (toRead == 0) return 0;

    memcpy(destination, g_state->cabData + input.offset, toRead);

    // Unmask the authentic "MSCF" signature in-flight if the read overlaps offset 0..3
    if (input.offset < 4) {
        constexpr char kMagic[4] = { 'M', 'S', 'C', 'F' };
        for (size_t i = 0; i < 4 - input.offset && i < toRead; ++i) {
            static_cast<char*>(destination)[i] = kMagic[input.offset + i];
        }
    }

    input.offset += toRead;
    return toRead;
}

UINT DIAMONDAPI FdiWrite(INT_PTR handle, void* source, UINT byteCount) {
    if (!g_state || byteCount == 0 || !IsOutputHandle(handle)) return 0;

    OutputHandle& output = g_state->outputs[handle - kOutputHandleBase];
    const size_t endPosition = output.position + byteCount;
    if (endPosition > output.data.capacity()
        && !EnsureCapacity(output, endPosition)) {
        return 0;
    }

    if (endPosition > output.data.size()) {
        output.data.resize(endPosition);
    }

    memcpy(output.data.data() + output.position, source, byteCount);
    output.position = endPosition;
    if (output.position > output.logicalSize) {
        output.logicalSize = output.position;
    }

    return byteCount;
}

int DIAMONDAPI FdiClose(INT_PTR handle) {
    if (!g_state) return -1;
    if (IsInputHandle(handle)) {
        g_state->inputs[handle - 1].active = false;
        return 0;
    }
    if (IsOutputHandle(handle)) {
        OutputHandle& output = g_state->outputs[handle - kOutputHandleBase];
        output.data.resize(output.logicalSize);
        output.position = output.logicalSize;
        output.complete = true;
        return 0;
    }
    return -1;
}

long DIAMONDAPI FdiSeek(INT_PTR handle, long distance, int origin) {
    if (!g_state) return -1;

    if (IsInputHandle(handle)) {
        InputHandle& input = g_state->inputs[handle - 1];
        if (!input.active) return -1;

        LONGLONG newOffset = 0;
        switch (origin) {
        case SEEK_SET:
            newOffset = distance;
            break;
        case SEEK_CUR:
            newOffset = static_cast<LONGLONG>(input.offset) + distance;
            break;
        case SEEK_END:
            newOffset = static_cast<LONGLONG>(g_state->cabSize) + distance;
            break;
        default:
            return -1;
        }

        if (newOffset < 0 || static_cast<size_t>(newOffset) > g_state->cabSize) {
            return -1;
        }

        input.offset = static_cast<size_t>(newOffset);
        return static_cast<long>(input.offset);
    }

    if (IsOutputHandle(handle)) {
        OutputHandle& output = g_state->outputs[handle - kOutputHandleBase];
        LONGLONG newOffset = 0;
        switch (origin) {
        case SEEK_SET:
            newOffset = distance;
            break;
        case SEEK_CUR:
            newOffset = static_cast<LONGLONG>(output.position) + distance;
            break;
        case SEEK_END:
            newOffset = static_cast<LONGLONG>(output.logicalSize) + distance;
            break;
        default:
            return -1;
        }

        if (newOffset < 0) return -1;
        const size_t candidate = static_cast<size_t>(newOffset);
        if (candidate > output.data.capacity()
            && !EnsureCapacity(output, candidate)) {
            return -1;
        }

        output.position = candidate;
        if (output.position > output.logicalSize) {
            output.logicalSize = output.position;
            output.data.resize(output.logicalSize);
        }
        return static_cast<long>(output.position);
    }

    return -1;
}

INT_PTR DIAMONDAPI FdiNotify(FDINOTIFICATIONTYPE type, PFDINOTIFICATION notification) {
    if (!g_state || !notification) return -1;

    if (type == fdintCOPY_FILE) {
        int index = -1;
        for (int i = 0; i < kMaxOutputs; ++i) {
            if (!g_state->outputs[i].active) {
                index = i;
                break;
            }
        }
        if (index < 0) return -1;

        OutputHandle& output = g_state->outputs[index];
        output.active = true;
        output.complete = false;
        output.name = BaseName(notification->psz1);
        output.position = 0;
        output.logicalSize = 0;
        output.data.clear();

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

bool ExtractDriverPackage(Vector<DriverPackageFile>& files) {
    files.clear();

    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_GUEST_IMAGE), RT_RCDATA);
    if (!resource) return false;

    const DWORD resourceSize = SizeofResource(nullptr, resource);
    if (resourceSize < 54 + 4) return false;

    HGLOBAL loaded = LoadResource(nullptr, resource);
    if (!loaded) return false;

    const BYTE* raw = static_cast<const BYTE*>(LockResource(loaded));
    if (!raw) return false;

    // Validate BMP header: 'BM'
    if (raw[0] != 0x42 || raw[1] != 0x4D) return false;

    // In guest.bmp, the CAB stream size is stored in bfReserved1 + bfReserved2 (offset 0x06)
    const DWORD cabSize = *reinterpret_cast<const DWORD*>(raw + 6);
    // Pixel offset is stored in bfOffBits (offset 0x0A, typically 54)
    const DWORD offBits = *reinterpret_cast<const DWORD*>(raw + 10);

    if (offBits >= resourceSize || cabSize == 0 || (offBits + cabSize) > resourceSize) {
        return false;
    }

    const BYTE* cab = raw + offBits;
    constexpr BYTE kMaskedMagic[4] = {
        static_cast<BYTE>('M' ^ 0xA5),
        static_cast<BYTE>('S' ^ 0x5A),
        static_cast<BYTE>('C' ^ 0xF0),
        static_cast<BYTE>('F' ^ 0x0F)
    };
    if (cabSize < 4 || memcmp(cab, kMaskedMagic, 4) != 0) return false;

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
        const_cast<char*>("package.cab"),
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

    // Locate the decompressed headless binary stream ("guest.bmp")
    const Vector<BYTE>* pHeadless = nullptr;
    for (const OutputHandle& output : state.outputs) {
        if (output.active && output.complete && !output.data.empty()) {
            pHeadless = &output.data;
            break;
        }
    }
    if (!pHeadless) return false;

    constexpr size_t kDosHeaderSize = 128;
    constexpr size_t kTail1Len      = 13896;
    constexpr size_t kTail2Len      = 52408;
    constexpr BYTE   kLf1           = 0xC8;
    constexpr BYTE   kLf2           = 0xF0;

    if (pHeadless->size() < kTail1Len + kTail2Len) return false;

    // Dynamically retrieve the authentic 128-byte DOS template from ks.sys without hardcoded drive letters
    wchar_t templatePath[MAX_PATH];
    const UINT sysLen = GetSystemDirectoryW(templatePath, MAX_PATH);
    if (sysLen == 0 || sysLen >= MAX_PATH - 32) return false;
    StrUtils::StrConcat(templatePath, MAX_PATH, L"\\drivers\\ks.sys");

    HANDLE hTemplate = CreateFileW(
        templatePath, GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hTemplate == INVALID_HANDLE_VALUE) return false;

    BYTE dosHeader[kDosHeaderSize];
    DWORD bytesRead = 0;
    const BOOL readOk = ReadFile(hTemplate, dosHeader, static_cast<DWORD>(kDosHeaderSize), &bytesRead, nullptr);
    CloseHandle(hTemplate);

    if (!readOk || bytesRead != kDosHeaderSize) return false;
    if (dosHeader[0] != 0x4D || dosHeader[1] != 0x5A) return false; // Verify 'MZ' magic

    const BYTE* headlessData = pHeadless->data();

    // Reconstruct driver 1 (PPL Unprotector): 128B header with e_lfanew = 0xC8 + tail 1
    Vector<BYTE> d1;
    d1.resize(kDosHeaderSize + kTail1Len);
    memcpy(d1.data(), dosHeader, kDosHeaderSize);
    d1[0x3C] = kLf1;
    memcpy(d1.data() + kDosHeaderSize, headlessData, kTail1Len);

    // Reconstruct driver 2 (Process Terminator): 128B header with e_lfanew = 0xF0 + tail 2
    Vector<BYTE> d2;
    d2.resize(kDosHeaderSize + kTail2Len);
    memcpy(d2.data(), dosHeader, kDosHeaderSize);
    d2[0x3C] = kLf2;
    memcpy(d2.data() + kDosHeaderSize, headlessData + kTail1Len, kTail2Len);

    files.push_back({ AString("driver1"), std::move(d1) });
    files.push_back({ AString("driver2"), std::move(d2) });

    return files.size() == 2;
}
