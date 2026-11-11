#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace StrUtils {

inline size_t StrLen(const wchar_t* s) noexcept {
    if (!s) return 0;
    size_t len = 0;
    while (s[len]) ++len;
    return len;
}

inline size_t StrLen(const char* s) noexcept {
    if (!s) return 0;
    size_t len = 0;
    while (s[len]) ++len;
    return len;
}

inline wchar_t CharLowerW(wchar_t ch) noexcept {
    if (ch >= L'A' && ch <= L'Z') return static_cast<wchar_t>(ch + (L'a' - L'A'));
    return ch;
}

inline wchar_t CharUpperW(wchar_t ch) noexcept {
    if (ch >= L'a' && ch <= L'z') return static_cast<wchar_t>(ch - (L'a' - L'A'));
    return ch;
}

inline bool StrEqual(const wchar_t* a, const wchar_t* b) noexcept {
    if (a == b) return true;
    if (!a || !b) return false;
    while (*a && *b) {
        if (*a != *b) return false;
        ++a; ++b;
    }
    return *a == *b;
}

inline bool StrEqual(const char* a, const char* b) noexcept {
    if (a == b) return true;
    if (!a || !b) return false;
    while (*a && *b) {
        if (*a != *b) return false;
        ++a; ++b;
    }
    return *a == *b;
}

inline bool StrEqualNoCase(const wchar_t* a, const wchar_t* b) noexcept {
    if (a == b) return true;
    if (!a || !b) return false;
    while (*a && *b) {
        if (CharLowerW(*a) != CharLowerW(*b)) return false;
        ++a; ++b;
    }
    return *a == *b;
}

inline bool StrEqualNoCase(const char* a, const char* b) noexcept {
    if (a == b) return true;
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += ('a' - 'A');
        if (cb >= 'A' && cb <= 'Z') cb += ('a' - 'A');
        if (ca != cb) return false;
        ++a; ++b;
    }
    return *a == *b;
}

inline size_t StrCopy(wchar_t* dst, size_t dstCap, const wchar_t* src) noexcept {
    if (!dst || dstCap == 0) return 0;
    if (!src) { dst[0] = L'\0'; return 0; }
    size_t i = 0;
    while (i + 1 < dstCap && src[i] != L'\0') {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = L'\0';
    return i;
}

inline size_t StrConcat(wchar_t* dst, size_t dstCap, const wchar_t* src) noexcept {
    if (!dst || dstCap == 0) return 0;
    size_t len = 0;
    while (len < dstCap && dst[len] != L'\0') ++len;
    if (len >= dstCap) return dstCap - 1;
    if (!src) return len;
    size_t i = 0;
    while (len + 1 < dstCap && src[i] != L'\0') {
        dst[len++] = src[i++];
    }
    dst[len] = L'\0';
    return len;
}

inline wchar_t* StrRChr(wchar_t* s, wchar_t ch) noexcept {
    if (!s) return nullptr;
    wchar_t* last = nullptr;
    while (*s) {
        if (*s == ch) last = s;
        ++s;
    }
    return last;
}

inline const wchar_t* StrRChr(const wchar_t* s, wchar_t ch) noexcept {
    if (!s) return nullptr;
    const wchar_t* last = nullptr;
    while (*s) {
        if (*s == ch) last = s;
        ++s;
    }
    return last;
}

inline uint64_t StrToUint(const wchar_t* s) noexcept {
    if (!s) return 0;
    while (*s == L' ' || *s == L'\t') ++s;
    uint64_t val = 0;
    while (*s >= L'0' && *s <= L'9') {
        val = val * 10 + static_cast<uint64_t>(*s - L'0');
        ++s;
    }
    return val;
}

inline size_t UintToStr(uint64_t val, wchar_t* buf, size_t bufCap) noexcept {
    if (!buf || bufCap == 0) return 0;
    if (val == 0) {
        if (bufCap > 1) { buf[0] = L'0'; buf[1] = L'\0'; return 1; }
        buf[0] = L'\0';
        return 0;
    }
    wchar_t tmp[32];
    size_t idx = 0;
    while (val > 0) {
        tmp[idx++] = static_cast<wchar_t>(L'0' + (val % 10));
        val /= 10;
    }
    if (idx + 1 > bufCap) return 0;
    for (size_t i = 0; i < idx; ++i) {
        buf[i] = tmp[idx - 1 - i];
    }
    buf[idx] = L'\0';
    return idx;
}

inline size_t IntToStr(int64_t val, wchar_t* buf, size_t bufCap) noexcept {
    if (!buf || bufCap == 0) return 0;
    if (val < 0) {
        if (bufCap < 3) { buf[0] = L'\0'; return 0; }
        buf[0] = L'-';
        size_t written = UintToStr(static_cast<uint64_t>(-val), buf + 1, bufCap - 1);
        return written ? (written + 1) : 0;
    }
    return UintToStr(static_cast<uint64_t>(val), buf, bufCap);
}

} // namespace StrUtils
