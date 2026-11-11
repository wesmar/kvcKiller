#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <span>
#include <utility>
#include "RuntimeSupport.h"
#include "StrUtils.h"

// =============================================================================
// Pair
// =============================================================================

template <typename T1, typename T2>
struct Pair {
    T1 first;
    T2 second;

    constexpr Pair() = default;
    constexpr Pair(const T1& a, const T2& b) : first(a), second(b) {}
    constexpr Pair(T1&& a, T2&& b) : first(std::move(a)), second(std::move(b)) {}
    constexpr Pair(const T1& a, T2&& b) : first(a), second(std::move(b)) {}
    constexpr Pair(T1&& a, const T2& b) : first(std::move(a)), second(b) {}
};

// =============================================================================
// Vector<T>
// =============================================================================

template <typename T>
class Vector {
    T*     data_     = nullptr;
    size_t size_     = 0;
    size_t capacity_ = 0;

    void destroy_elements(size_t from, size_t to) noexcept {
        for (size_t i = from; i < to; ++i) {
            data_[i].~T();
        }
    }

public:
    Vector() = default;

    explicit Vector(size_t count, const T& value = T()) {
        resize(count, value);
    }

    ~Vector() noexcept {
        clear();
        if (data_) {
            HeapFree(GetProcessHeap(), 0, data_);
        }
    }

    Vector(const Vector& other) {
        if (other.size_ > 0) {
            reserve(other.size_);
            for (size_t i = 0; i < other.size_; ++i) {
                new (static_cast<void*>(data_ + i)) T(other.data_[i]);
            }
            size_ = other.size_;
        }
    }

    Vector& operator=(const Vector& other) {
        if (this != &other) {
            clear();
            if (other.size_ > 0) {
                reserve(other.size_);
                for (size_t i = 0; i < other.size_; ++i) {
                    new (static_cast<void*>(data_ + i)) T(other.data_[i]);
                }
                size_ = other.size_;
            }
        }
        return *this;
    }

    Vector(Vector&& other) noexcept
        : data_(other.data_), size_(other.size_), capacity_(other.capacity_) {
        other.data_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
    }

    Vector& operator=(Vector&& other) noexcept {
        if (this != &other) {
            clear();
            if (data_) HeapFree(GetProcessHeap(), 0, data_);
            data_ = other.data_;
            size_ = other.size_;
            capacity_ = other.capacity_;
            other.data_ = nullptr;
            other.size_ = 0;
            other.capacity_ = 0;
        }
        return *this;
    }

    bool reserve(size_t newCap) noexcept {
        if (newCap <= capacity_) return true;
        HANDLE h = GetProcessHeap();
        T* newData = static_cast<T*>(HeapAlloc(h, 0, newCap * sizeof(T)));
        if (!newData) return false;

        for (size_t i = 0; i < size_; ++i) {
            new (static_cast<void*>(newData + i)) T(std::move(data_[i]));
            data_[i].~T();
        }
        if (data_) HeapFree(h, 0, data_);
        data_ = newData;
        capacity_ = newCap;
        return true;
    }

    bool resize(size_t newSize, const T& value = T()) noexcept {
        if (newSize > capacity_) {
            size_t newCap = capacity_ ? (capacity_ * 3) / 2 + 8 : 8;
            if (newCap < newSize) newCap = newSize;
            if (!reserve(newCap)) return false;
        }
        if (newSize > size_) {
            for (size_t i = size_; i < newSize; ++i) {
                new (static_cast<void*>(data_ + i)) T(value);
            }
        } else if (newSize < size_) {
            destroy_elements(newSize, size_);
        }
        size_ = newSize;
        return true;
    }

    void assign(size_t count, const T& value) {
        clear();
        resize(count, value);
    }

    bool push_back(const T& item) noexcept {
        if (size_ >= capacity_) {
            size_t newCap = capacity_ ? capacity_ * 2 : 8;
            if (!reserve(newCap)) return false;
        }
        new (static_cast<void*>(data_ + size_)) T(item);
        ++size_;
        return true;
    }

    bool push_back(T&& item) noexcept {
        if (size_ >= capacity_) {
            size_t newCap = capacity_ ? capacity_ * 2 : 8;
            if (!reserve(newCap)) return false;
        }
        new (static_cast<void*>(data_ + size_)) T(std::move(item));
        ++size_;
        return true;
    }

    template <typename... Args>
    T& emplace_back(Args&&... args) noexcept {
        if (size_ >= capacity_) {
            size_t newCap = capacity_ ? capacity_ * 2 : 8;
            reserve(newCap);
        }
        new (static_cast<void*>(data_ + size_)) T(std::forward<Args>(args)...);
        return data_[size_++];
    }

    void pop_back() noexcept {
        if (size_ > 0) {
            --size_;
            data_[size_].~T();
        }
    }

    void clear() noexcept {
        destroy_elements(0, size_);
        size_ = 0;
    }

    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] T* data() noexcept { return data_; }
    [[nodiscard]] const T* data() const noexcept { return data_; }
    [[nodiscard]] std::span<T> span() noexcept { return std::span<T>(data_, size_); }
    [[nodiscard]] std::span<const T> span() const noexcept { return std::span<const T>(data_, size_); }

    T& operator[](size_t idx) noexcept { return data_[idx]; }
    const T& operator[](size_t idx) const noexcept { return data_[idx]; }

    T& front() noexcept { return data_[0]; }
    const T& front() const noexcept { return data_[0]; }
    T& back() noexcept { return data_[size_ - 1]; }
    const T& back() const noexcept { return data_[size_ - 1]; }

    T* begin() noexcept { return data_; }
    T* end() noexcept { return data_ + size_; }
    const T* begin() const noexcept { return data_; }
    const T* end() const noexcept { return data_ + size_; }
    const T* cbegin() const noexcept { return data_; }
    const T* cend() const noexcept { return data_ + size_; }
};

// =============================================================================
// WString (lightweight Unicode string with SSO)
// =============================================================================

class WString {
public:
    static constexpr size_t npos = static_cast<size_t>(-1);
    static constexpr size_t kSsoCap = 24;

private:
    size_t   size_     = 0;
    size_t   capacity_ = kSsoCap - 1;
    wchar_t* ptr_      = sso_;
    wchar_t  sso_[kSsoCap];

    bool is_sso() const noexcept { return ptr_ == sso_; }

    void set_buffer(wchar_t* newPtr, size_t newCap) noexcept {
        if (!is_sso() && ptr_) {
            HeapFree(GetProcessHeap(), 0, ptr_);
        }
        ptr_ = newPtr;
        capacity_ = newCap;
    }

public:
    WString() noexcept {
        sso_[0] = L'\0';
    }

    WString(const wchar_t* str) {
        sso_[0] = L'\0';
        if (str) {
            assign(str, StrUtils::StrLen(str));
        }
    }

    WString(const wchar_t* str, size_t len) {
        sso_[0] = L'\0';
        assign(str, len);
    }

    WString(size_t count, wchar_t ch) {
        sso_[0] = L'\0';
        resize(count, ch);
    }

    WString(std::wstring_view sv) {
        sso_[0] = L'\0';
        assign(sv.data(), sv.size());
    }

    ~WString() noexcept {
        if (!is_sso() && ptr_) {
            HeapFree(GetProcessHeap(), 0, ptr_);
        }
    }

    WString(const WString& other) {
        sso_[0] = L'\0';
        assign(other.data(), other.size());
    }

    WString& operator=(const WString& other) {
        if (this != &other) {
            assign(other.data(), other.size());
        }
        return *this;
    }

    WString(WString&& other) noexcept {
        if (other.is_sso()) {
            size_ = other.size_;
            capacity_ = kSsoCap - 1;
            ptr_ = sso_;
            memcpy(sso_, other.sso_, (size_ + 1) * sizeof(wchar_t));
        } else {
            size_ = other.size_;
            capacity_ = other.capacity_;
            ptr_ = other.ptr_;
        }
        other.ptr_ = other.sso_;
        other.size_ = 0;
        other.capacity_ = kSsoCap - 1;
        other.sso_[0] = L'\0';
    }

    WString& operator=(WString&& other) noexcept {
        if (this != &other) {
            if (!is_sso() && ptr_) {
                HeapFree(GetProcessHeap(), 0, ptr_);
            }
            if (other.is_sso()) {
                size_ = other.size_;
                capacity_ = kSsoCap - 1;
                ptr_ = sso_;
                memcpy(sso_, other.sso_, (size_ + 1) * sizeof(wchar_t));
            } else {
                size_ = other.size_;
                capacity_ = other.capacity_;
                ptr_ = other.ptr_;
            }
            other.ptr_ = other.sso_;
            other.size_ = 0;
            other.capacity_ = kSsoCap - 1;
            other.sso_[0] = L'\0';
        }
        return *this;
    }

    WString& operator=(const wchar_t* str) {
        assign(str ? str : L"", str ? StrUtils::StrLen(str) : 0);
        return *this;
    }

    WString& operator=(std::wstring_view sv) {
        assign(sv.data(), sv.size());
        return *this;
    }

    [[nodiscard]] const wchar_t* c_str() const noexcept { return ptr_; }
    [[nodiscard]] const wchar_t* data() const noexcept { return ptr_; }
    [[nodiscard]] wchar_t* data() noexcept { return ptr_; }
    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] size_t length() const noexcept { return size_; }
    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    wchar_t& operator[](size_t idx) noexcept { return ptr_[idx]; }
    const wchar_t& operator[](size_t idx) const noexcept { return ptr_[idx]; }
    wchar_t& front() noexcept { return ptr_[0]; }
    const wchar_t& front() const noexcept { return ptr_[0]; }
    wchar_t& back() noexcept { return ptr_[size_ ? size_ - 1 : 0]; }
    const wchar_t& back() const noexcept { return ptr_[size_ ? size_ - 1 : 0]; }

    wchar_t* begin() noexcept { return ptr_; }
    wchar_t* end() noexcept { return ptr_ + size_; }
    const wchar_t* begin() const noexcept { return ptr_; }
    const wchar_t* end() const noexcept { return ptr_ + size_; }
    const wchar_t* cbegin() const noexcept { return ptr_; }
    const wchar_t* cend() const noexcept { return ptr_ + size_; }

    operator std::wstring_view() const noexcept {
        return std::wstring_view(ptr_, size_);
    }

    void clear() noexcept {
        size_ = 0;
        ptr_[0] = L'\0';
    }

    bool reserve(size_t newCap) noexcept {
        if (newCap <= capacity_) return true;
        size_t allocCap = (newCap + 1) * sizeof(wchar_t);
        HANDLE h = GetProcessHeap();
        wchar_t* newPtr = static_cast<wchar_t*>(HeapAlloc(h, 0, allocCap));
        if (!newPtr) return false;

        memcpy(newPtr, ptr_, (size_ + 1) * sizeof(wchar_t));
        set_buffer(newPtr, newCap);
        return true;
    }

    void resize(size_t newSize, wchar_t ch = L'\0') {
        if (newSize > capacity_) {
            size_t newCap = capacity_ * 2 + 8;
            if (newCap < newSize) newCap = newSize;
            reserve(newCap);
        }
        if (newSize > size_) {
            for (size_t i = size_; i < newSize; ++i) {
                ptr_[i] = ch;
            }
        }
        size_ = newSize;
        ptr_[size_] = L'\0';
    }

    WString& assign(const wchar_t* str, size_t len) {
        if (!str || len == 0) {
            clear();
            return *this;
        }
        if (len > capacity_) {
            reserve(len);
        }
        memmove(ptr_, str, len * sizeof(wchar_t));
        size_ = len;
        ptr_[size_] = L'\0';
        return *this;
    }

    WString& assign(const wchar_t* str) {
        return assign(str, str ? StrUtils::StrLen(str) : 0);
    }

    WString& append(const wchar_t* str, size_t len) {
        if (!str || len == 0) return *this;
        const size_t req = size_ + len;
        if (req > capacity_) {
            size_t newCap = capacity_ * 2 + 8;
            if (newCap < req) newCap = req;
            reserve(newCap);
        }
        memcpy(ptr_ + size_, str, len * sizeof(wchar_t));
        size_ = req;
        ptr_[size_] = L'\0';
        return *this;
    }

    WString& append(const wchar_t* str) {
        return append(str, str ? StrUtils::StrLen(str) : 0);
    }

    WString& append(const WString& other) {
        return append(other.data(), other.size());
    }

    WString& append(std::wstring_view sv) {
        return append(sv.data(), sv.size());
    }

    WString& append(size_t count, wchar_t ch) {
        const size_t req = size_ + count;
        if (req > capacity_) {
            size_t newCap = capacity_ * 2 + 8;
            if (newCap < req) newCap = req;
            reserve(newCap);
        }
        for (size_t i = 0; i < count; ++i) {
            ptr_[size_ + i] = ch;
        }
        size_ = req;
        ptr_[size_] = L'\0';
        return *this;
    }

    void push_back(wchar_t ch) {
        if (size_ + 1 > capacity_) {
            reserve(capacity_ * 2 + 8);
        }
        ptr_[size_++] = ch;
        ptr_[size_] = L'\0';
    }

    void pop_back() {
        if (size_ > 0) {
            --size_;
            ptr_[size_] = L'\0';
        }
    }

    WString& operator+=(const WString& other) { return append(other); }
    WString& operator+=(const wchar_t* str) { return append(str); }
    WString& operator+=(std::wstring_view sv) { return append(sv); }
    WString& operator+=(wchar_t ch) { push_back(ch); return *this; }

    [[nodiscard]] WString substr(size_t pos = 0, size_t count = npos) const {
        if (pos >= size_) return WString();
        const size_t avail = size_ - pos;
        const size_t take = (count < avail) ? count : avail;
        return WString(ptr_ + pos, take);
    }

    [[nodiscard]] size_t find(wchar_t ch, size_t pos = 0) const noexcept {
        for (size_t i = pos; i < size_; ++i) {
            if (ptr_[i] == ch) return i;
        }
        return npos;
    }

    [[nodiscard]] size_t find(const wchar_t* str, size_t pos = 0) const noexcept {
        if (!str) return npos;
        size_t slen = StrUtils::StrLen(str);
        if (slen == 0) return (pos <= size_) ? pos : npos;
        if (pos + slen > size_) return npos;
        for (size_t i = pos; i + slen <= size_; ++i) {
            if (memcmp(ptr_ + i, str, slen * sizeof(wchar_t)) == 0) return i;
        }
        return npos;
    }

    [[nodiscard]] size_t find_first_of(const wchar_t* chars, size_t pos = 0) const noexcept {
        if (!chars || pos >= size_) return npos;
        for (size_t i = pos; i < size_; ++i) {
            const wchar_t c = ptr_[i];
            for (const wchar_t* p = chars; *p; ++p) {
                if (*p == c) return i;
            }
        }
        return npos;
    }

    [[nodiscard]] size_t find_first_not_of(const wchar_t* chars, size_t pos = 0) const noexcept {
        if (!chars) return pos < size_ ? pos : npos;
        for (size_t i = pos; i < size_; ++i) {
            const wchar_t c = ptr_[i];
            bool found = false;
            for (const wchar_t* p = chars; *p; ++p) {
                if (*p == c) { found = true; break; }
            }
            if (!found) return i;
        }
        return npos;
    }

    [[nodiscard]] size_t find_last_not_of(const wchar_t* chars, size_t pos = npos) const noexcept {
        if (size_ == 0) return npos;
        size_t i = (pos >= size_) ? (size_ - 1) : pos;
        while (true) {
            const wchar_t c = ptr_[i];
            bool found = false;
            for (const wchar_t* p = chars; *p; ++p) {
                if (*p == c) { found = true; break; }
            }
            if (!found) return i;
            if (i == 0) break;
            --i;
        }
        return npos;
    }

    [[nodiscard]] size_t find_last_of(const wchar_t* chars, size_t pos = npos) const noexcept {
        if (size_ == 0 || !chars) return npos;
        size_t i = (pos >= size_) ? (size_ - 1) : pos;
        while (true) {
            const wchar_t c = ptr_[i];
            for (const wchar_t* p = chars; *p; ++p) {
                if (*p == c) return i;
            }
            if (i == 0) break;
            --i;
        }
        return npos;
    }

    [[nodiscard]] size_t rfind(wchar_t ch, size_t pos = npos) const noexcept {
        if (size_ == 0) return npos;
        size_t i = (pos >= size_) ? (size_ - 1) : pos;
        while (true) {
            if (ptr_[i] == ch) return i;
            if (i == 0) break;
            --i;
        }
        return npos;
    }

    [[nodiscard]] bool starts_with(std::wstring_view sv) const noexcept {
        if (sv.size() > size_) return false;
        return memcmp(ptr_, sv.data(), sv.size() * sizeof(wchar_t)) == 0;
    }

    [[nodiscard]] bool ends_with(std::wstring_view sv) const noexcept {
        if (sv.size() > size_) return false;
        return memcmp(ptr_ + size_ - sv.size(), sv.data(), sv.size() * sizeof(wchar_t)) == 0;
    }
};

inline WString operator+(const WString& a, const WString& b) {
    WString res;
    res.reserve(a.size() + b.size());
    res.append(a);
    res.append(b);
    return res;
}

inline WString operator+(const WString& a, const wchar_t* b) {
    WString res;
    size_t blen = StrUtils::StrLen(b);
    res.reserve(a.size() + blen);
    res.append(a);
    res.append(b, blen);
    return res;
}

inline WString operator+(const wchar_t* a, const WString& b) {
    WString res;
    size_t alen = StrUtils::StrLen(a);
    res.reserve(alen + b.size());
    res.append(a, alen);
    res.append(b);
    return res;
}

inline WString operator+(const WString& a, wchar_t b) {
    WString res = a;
    res.push_back(b);
    return res;
}

inline bool operator==(const WString& a, const WString& b) noexcept {
    if (a.size() != b.size()) return false;
    return memcmp(a.data(), b.data(), a.size() * sizeof(wchar_t)) == 0;
}

inline bool operator==(const WString& a, const wchar_t* b) noexcept {
    if (!b) return a.empty();
    return StrUtils::StrEqual(a.c_str(), b);
}

inline bool operator==(const wchar_t* a, const WString& b) noexcept {
    return b == a;
}

inline bool operator!=(const WString& a, const WString& b) noexcept { return !(a == b); }
inline bool operator!=(const WString& a, const wchar_t* b) noexcept { return !(a == b); }
inline bool operator!=(const wchar_t* a, const WString& b) noexcept { return !(a == b); }

inline bool operator<(const WString& a, const WString& b) noexcept {
    size_t minLen = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < minLen; ++i) {
        if (a[i] != b[i]) return a[i] < b[i];
    }
    return a.size() < b.size();
}

// =============================================================================
// AString (lightweight 8-bit string)
// =============================================================================

class AString {
    size_t capacity_ = 15;
    size_t size_ = 0;
    char*  ptr_ = sso_;
    char   sso_[16];

    bool is_sso() const noexcept { return ptr_ == sso_; }

public:
    AString() noexcept { sso_[0] = '\0'; }

    AString(const char* s) {
        sso_[0] = '\0';
        if (s) assign(s, StrUtils::StrLen(s));
    }

    AString(const char* s, size_t len) {
        sso_[0] = '\0';
        assign(s, len);
    }

    ~AString() noexcept {
        if (!is_sso() && ptr_) HeapFree(GetProcessHeap(), 0, ptr_);
    }

    AString(const AString& o) {
        sso_[0] = '\0';
        assign(o.data(), o.size());
    }

    AString& operator=(const AString& o) {
        if (this != &o) assign(o.data(), o.size());
        return *this;
    }

    AString(AString&& o) noexcept {
        if (o.is_sso()) {
            size_ = o.size_;
            capacity_ = 15;
            ptr_ = sso_;
            memcpy(sso_, o.sso_, size_ + 1);
        } else {
            size_ = o.size_;
            capacity_ = o.capacity_;
            ptr_ = o.ptr_;
        }
        o.ptr_ = o.sso_;
        o.size_ = 0;
        o.capacity_ = 15;
        o.sso_[0] = '\0';
    }

    AString& operator=(AString&& o) noexcept {
        if (this != &o) {
            if (!is_sso() && ptr_) HeapFree(GetProcessHeap(), 0, ptr_);
            if (o.is_sso()) {
                size_ = o.size_;
                capacity_ = 15;
                ptr_ = sso_;
                memcpy(sso_, o.sso_, size_ + 1);
            } else {
                size_ = o.size_;
                capacity_ = o.capacity_;
                ptr_ = o.ptr_;
            }
            o.ptr_ = o.sso_;
            o.size_ = 0;
            o.capacity_ = 15;
            o.sso_[0] = '\0';
        }
        return *this;
    }

    void assign(const char* s) {
        assign(s, s ? StrUtils::StrLen(s) : 0);
    }

    void assign(const char* s, size_t len) {
        if (!s || len == 0) {
            clear();
            return;
        }
        if (len > capacity_) {
            HANDLE h = GetProcessHeap();
            char* p = static_cast<char*>(HeapAlloc(h, 0, len + 1));
            if (!p) return;
            if (!is_sso() && ptr_) HeapFree(h, 0, ptr_);
            ptr_ = p;
            capacity_ = len;
        }
        memcpy(ptr_, s, len);
        size_ = len;
        ptr_[size_] = '\0';
    }

    void clear() noexcept {
        size_ = 0;
        ptr_[0] = '\0';
    }

    [[nodiscard]] const char* c_str() const noexcept { return ptr_; }
    [[nodiscard]] const char* data() const noexcept { return ptr_; }
    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
};

// =============================================================================
// Map<Key, Value> (flat sorted vector map)
// =============================================================================

template <typename Key, typename Value>
class Map {
    Vector<Pair<Key, Value>> items_;

    size_t lower_bound_index(const Key& k) const noexcept {
        size_t low = 0;
        size_t high = items_.size();
        while (low < high) {
            size_t mid = low + (high - low) / 2;
            if (items_[mid].first < k) {
                low = mid + 1;
            } else {
                high = mid;
            }
        }
        return low;
    }

public:
    Map() = default;

    Pair<Key, Value>* begin() noexcept { return items_.begin(); }
    Pair<Key, Value>* end() noexcept { return items_.end(); }
    const Pair<Key, Value>* begin() const noexcept { return items_.begin(); }
    const Pair<Key, Value>* end() const noexcept { return items_.end(); }
    const Pair<Key, Value>* cbegin() const noexcept { return items_.cbegin(); }
    const Pair<Key, Value>* cend() const noexcept { return items_.cend(); }

    [[nodiscard]] size_t size() const noexcept { return items_.size(); }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
    void clear() noexcept { items_.clear(); }

    Pair<Key, Value>* find(const Key& k) noexcept {
        size_t idx = lower_bound_index(k);
        if (idx < items_.size() && !(k < items_[idx].first)) {
            return &items_[idx];
        }
        return end();
    }

    const Pair<Key, Value>* find(const Key& k) const noexcept {
        size_t idx = lower_bound_index(k);
        if (idx < items_.size() && !(k < items_[idx].first)) {
            return &items_[idx];
        }
        return end();
    }

    [[nodiscard]] bool contains(const Key& k) const noexcept {
        return find(k) != end();
    }

    Value& operator[](const Key& k) {
        size_t idx = lower_bound_index(k);
        if (idx < items_.size() && !(k < items_[idx].first)) {
            return items_[idx].second;
        }
        // Insert at idx
        items_.push_back(Pair<Key, Value>(k, Value()));
        for (size_t i = items_.size() - 1; i > idx; --i) {
            std::swap(items_[i], items_[i - 1]);
        }
        return items_[idx].second;
    }

    size_t erase(const Key& k) noexcept {
        size_t idx = lower_bound_index(k);
        if (idx < items_.size() && !(k < items_[idx].first)) {
            for (size_t i = idx; i + 1 < items_.size(); ++i) {
                items_[i] = std::move(items_[i + 1]);
            }
            items_.pop_back();
            return 1;
        }
        return 0;
    }
};

// =============================================================================
// Set<Key> (flat sorted vector set)
// =============================================================================

template <typename Key>
class Set {
    Vector<Key> items_;

    size_t lower_bound_index(const Key& k) const noexcept {
        size_t low = 0;
        size_t high = items_.size();
        while (low < high) {
            size_t mid = low + (high - low) / 2;
            if (items_[mid] < k) {
                low = mid + 1;
            } else {
                high = mid;
            }
        }
        return low;
    }

public:
    Set() = default;

    Key* begin() noexcept { return items_.begin(); }
    Key* end() noexcept { return items_.end(); }
    const Key* begin() const noexcept { return items_.begin(); }
    const Key* end() const noexcept { return items_.end(); }
    const Key* cbegin() const noexcept { return items_.cbegin(); }
    const Key* cend() const noexcept { return items_.cend(); }

    [[nodiscard]] size_t size() const noexcept { return items_.size(); }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
    void clear() noexcept { items_.clear(); }

    bool insert(const Key& k) {
        size_t idx = lower_bound_index(k);
        if (idx < items_.size() && !(k < items_[idx])) {
            return false; // already present
        }
        items_.push_back(k);
        for (size_t i = items_.size() - 1; i > idx; --i) {
            std::swap(items_[i], items_[i - 1]);
        }
        return true;
    }

    [[nodiscard]] bool contains(const Key& k) const noexcept {
        size_t idx = lower_bound_index(k);
        return idx < items_.size() && !(k < items_[idx]);
    }

    [[nodiscard]] size_t count(const Key& k) const noexcept {
        return contains(k) ? 1 : 0;
    }

    size_t erase(const Key& k) noexcept {
        size_t idx = lower_bound_index(k);
        if (idx < items_.size() && !(k < items_[idx])) {
            for (size_t i = idx; i + 1 < items_.size(); ++i) {
                items_[i] = std::move(items_[i + 1]);
            }
            items_.pop_back();
            return 1;
        }
        return 0;
    }
};

// =============================================================================
// StableSort (Insertion sort - compact code size, stable)
// =============================================================================

template <typename T, typename Compare>
void StableSort(T* first, T* last, Compare comp) {
    if (first >= last) return;
    for (T* i = first + 1; i < last; ++i) {
        T val = std::move(*i);
        T* j = i;
        while (j > first && comp(val, *(j - 1))) {
            *j = std::move(*(j - 1));
            --j;
        }
        *j = std::move(val);
    }
}

// =============================================================================
// SrwLockGuard
// =============================================================================

class SrwLockGuard {
    SRWLOCK* lock_;
public:
    explicit SrwLockGuard(SRWLOCK& lock) noexcept : lock_(&lock) {
        AcquireSRWLockExclusive(lock_);
    }
    ~SrwLockGuard() noexcept {
        if (lock_) ReleaseSRWLockExclusive(lock_);
    }
    SrwLockGuard(const SrwLockGuard&) = delete;
    SrwLockGuard& operator=(const SrwLockGuard&) = delete;
};
