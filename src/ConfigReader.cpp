// ConfigReader.cpp
// Registry-backed configuration with embedded defaults seed.
// © WESMAR Marek Wesołowski
//
// All persistent state lives under HKCU\Software\KvcKiller:
//   Targets\List     REG_MULTI_SZ  — list of target process names (no extension)
//   Paths\<name.exe> REG_SZ        — last-known full path for a given process
//   History\Entries  REG_MULTI_SZ  — ring buffer of kill/restore log entries (max 16)
//   UI\              (reserved)
//
// On first run, ReadProcessList seeds the Targets key from the IDR_DEFAULT_TARGETS
// embedded resource (a .ini-style text file in [Targets] format).
//
// All public methods acquire g_registryMutex so they are safe to call from
// any thread.  Private helpers assume the lock is already held.

#include "ConfigReader.h"
#include "Resource.h"
#include <algorithm>
#include <cwctype>
#include <mutex>
#include <string>
#include <vector>

namespace {

// Registry key paths — all under HKEY_CURRENT_USER.
constexpr wchar_t kRegistryRoot[] = L"Software\\KvcKiller";
constexpr wchar_t kTargetsKey[]   = L"Software\\KvcKiller\\Targets";
constexpr wchar_t kPathsKey[]     = L"Software\\KvcKiller\\Paths";
constexpr wchar_t kHistoryKey[]   = L"Software\\KvcKiller\\History";
constexpr wchar_t kUiKey[]        = L"Software\\KvcKiller\\UI";

constexpr wchar_t kTargetsValue[] = L"List";
constexpr wchar_t kHistoryValue[] = L"Entries";
constexpr size_t  kMaxHistoryEntries = 16; // oldest entries are dropped when the ring fills

// Single mutex guards all registry access from this module.
std::mutex g_registryMutex;

// --- String utility functions (local to this TU) ---

// Returns a lowercase copy using wide-character towlower.
std::wstring ToLowerCopy(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return value;
}

// Strips leading/trailing whitespace characters (space, tab, CR, LF).
std::wstring TrimWhitespace(const std::wstring& value) {
    const size_t start = value.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) {
        return L"";
    }
    const size_t end = value.find_last_not_of(L" \t\r\n");
    return value.substr(start, end - start + 1);
}

// Trims whitespace and removes a trailing ".exe" suffix (case-insensitive).
// Used to normalize user-supplied names before storing them.
std::wstring StripExeExtension(const std::wstring& value) {
    const std::wstring trimmed = TrimWhitespace(value);
    const std::wstring lower   = ToLowerCopy(trimmed);
    if (lower.length() > 4 && lower.substr(lower.length() - 4) == L".exe") {
        return trimmed.substr(0, trimmed.length() - 4);
    }
    return trimmed;
}

// Produces a canonical registry value name for a process path entry:
//   "MsMpEng" → "msmpeng.exe"
//   "MsMpEng.exe" → "msmpeng.exe"
// This makes ReadProcessPath / WriteProcessPath case-insensitive and consistent
// regardless of whether the caller includes the extension.
std::wstring NormalizePathValueName(const std::wstring& processName) {
    const std::wstring stripped = StripExeExtension(processName);
    if (stripped.empty()) {
        return L"";
    }
    return ToLowerCopy(stripped) + L".exe";
}

// Creates the four registry subkeys if they don't already exist.
// Called at the start of every public method so the structure is always valid
// even on a first-run machine with no prior KvcKiller registry state.
bool EnsureRegistryScaffold() {
    const wchar_t* keys[] = { kRegistryRoot, kTargetsKey, kPathsKey, kHistoryKey, kUiKey };
    for (const wchar_t* keyPath : keys) {
        HKEY hKey = nullptr;
        const LONG status = RegCreateKeyExW(HKEY_CURRENT_USER, keyPath, 0, nullptr, 0,
                                            KEY_READ | KEY_WRITE, nullptr, &hKey, nullptr);
        if (status != ERROR_SUCCESS) {
            return false;
        }
        RegCloseKey(hKey);
    }
    return true;
}

// Decodes a raw byte buffer into a wide string, handling three encoding cases:
//   1. UTF-16 LE with BOM (0xFF 0xFE) — cast directly.
//   2. UTF-8 with BOM (0xEF 0xBB 0xBF) — skip the 3-byte BOM, then convert.
//   3. Plain UTF-8 or ACP — try UTF-8 first; fall back to system ANSI code page.
// Used for the embedded IDR_DEFAULT_TARGETS resource which may be saved in
// different encodings depending on the editor.
std::wstring DecodeTextBytes(const BYTE* data, size_t size) {
    if (data == nullptr || size == 0) {
        return L"";
    }

    // UTF-16 LE BOM
    if (size >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
        return std::wstring(reinterpret_cast<const wchar_t*>(data + 2),
                            (size - 2) / sizeof(wchar_t));
    }

    UINT codePage = CP_UTF8;
    DWORD flags   = MB_ERR_INVALID_CHARS;
    size_t offset = 0;

    // UTF-8 BOM — skip the 3-byte marker before conversion.
    if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        offset = 3;
    }

    int wideLen = MultiByteToWideChar(codePage, flags,
        reinterpret_cast<LPCCH>(data + offset),
        static_cast<int>(size - offset), nullptr, 0);

    if (wideLen <= 0) {
        // UTF-8 failed — fall back to the system ANSI code page.
        codePage = CP_ACP;
        flags    = 0;
        wideLen  = MultiByteToWideChar(codePage, flags,
            reinterpret_cast<LPCCH>(data + offset),
            static_cast<int>(size - offset), nullptr, 0);
    }

    if (wideLen <= 0) {
        return L"";
    }

    std::wstring text(wideLen, L'\0');
    MultiByteToWideChar(codePage, flags,
        reinterpret_cast<LPCCH>(data + offset),
        static_cast<int>(size - offset),
        text.data(), wideLen);
    return text;
}

// Parses a .ini-style process list text into a vector of bare process names
// (no .exe extension, no whitespace).
//
// Format:
//   ; comment
//   [Targets]
//   SomeName.exe       ← bare name or key=value both accepted
//   AnotherProcess
//
// Lines outside [Targets] sections and blank/comment lines are ignored.
std::vector<std::wstring> ParseProcessListText(const std::wstring& contents) {
    std::vector<std::wstring> processes;
    bool   inTargets = false;
    size_t cursor    = 0;

    while (cursor <= contents.size()) {
        const size_t next = contents.find_first_of(L"\r\n", cursor);
        std::wstring line = contents.substr(cursor,
            next == std::wstring::npos ? std::wstring::npos : next - cursor);

        if (next == std::wstring::npos) {
            cursor = contents.size() + 1;
        } else {
            cursor = next + 1;
            // Skip the \n in a \r\n pair.
            if (cursor < contents.size() && contents[next] == L'\r' && contents[cursor] == L'\n') {
                ++cursor;
            }
        }

        line = TrimWhitespace(line);
        if (line.empty() || line[0] == L';' || line[0] == L'#') {
            continue;
        }

        // Section header
        if (line[0] == L'[' && line.back() == L']') {
            inTargets = (line == L"[Targets]");
            continue;
        }

        if (!inTargets) {
            continue;
        }

        // Accept both "Name.exe" and "key=Name.exe" (ini assignment syntax).
        const size_t eq = line.find(L'=');
        if (eq != std::wstring::npos) {
            line = line.substr(eq + 1);
        }

        line = StripExeExtension(line);
        if (!line.empty()) {
            processes.push_back(line);
        }
    }

    return processes;
}

// --- Generic registry I/O helpers ---

// Reads a REG_MULTI_SZ value and splits it into individual strings.
// Returns an empty vector if the value is missing or empty.
std::vector<std::wstring> ReadMultiStringValue(const wchar_t* keyPath, const wchar_t* valueName) {
    DWORD bytes = 0;
    LONG status = RegGetValueW(HKEY_CURRENT_USER, keyPath, valueName, RRF_RT_REG_MULTI_SZ,
                               nullptr, nullptr, &bytes);
    if (status != ERROR_SUCCESS || bytes == 0) {
        return {};
    }

    std::vector<wchar_t> buffer((bytes / sizeof(wchar_t)) + 1, L'\0');
    status = RegGetValueW(HKEY_CURRENT_USER, keyPath, valueName, RRF_RT_REG_MULTI_SZ,
                          nullptr, buffer.data(), &bytes);
    if (status != ERROR_SUCCESS) {
        return {};
    }

    // REG_MULTI_SZ is a sequence of null-terminated strings followed by an extra
    // null — walk it string by string until we hit the double-null terminator.
    std::vector<std::wstring> values;
    const wchar_t* current = buffer.data();
    while (*current != L'\0') {
        values.emplace_back(current);
        current += wcslen(current) + 1;
    }

    return values;
}

// Serialises a vector of strings into a REG_MULTI_SZ value and writes it.
bool WriteMultiStringValue(const wchar_t* keyPath, const wchar_t* valueName,
                           const std::vector<std::wstring>& values) {
    HKEY hKey = nullptr;
    const LONG createStatus = RegCreateKeyExW(HKEY_CURRENT_USER, keyPath, 0, nullptr, 0,
                                              KEY_READ | KEY_WRITE, nullptr, &hKey, nullptr);
    if (createStatus != ERROR_SUCCESS) {
        return false;
    }

    // Build the double-null-terminated buffer.
    std::vector<wchar_t> buffer;
    size_t totalChars = 1; // for the final extra null
    for (const auto& value : values) {
        totalChars += value.size() + 1;
    }
    buffer.reserve(totalChars);

    for (const auto& value : values) {
        buffer.insert(buffer.end(), value.begin(), value.end());
        buffer.push_back(L'\0');
    }
    buffer.push_back(L'\0'); // double-null terminator

    const DWORD bytes  = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    const LONG  status = RegSetValueExW(hKey, valueName, 0, REG_MULTI_SZ,
                                        reinterpret_cast<const BYTE*>(buffer.data()), bytes);
    RegCloseKey(hKey);
    return status == ERROR_SUCCESS;
}

// Reads a single REG_SZ string value.  Returns an empty string if missing.
std::wstring ReadStringValue(const wchar_t* keyPath, const std::wstring& valueName) {
    DWORD bytes = 0;
    LONG status = RegGetValueW(HKEY_CURRENT_USER, keyPath, valueName.c_str(), RRF_RT_REG_SZ,
                               nullptr, nullptr, &bytes);
    if (status != ERROR_SUCCESS || bytes < sizeof(wchar_t)) {
        return L"";
    }

    std::vector<wchar_t> buffer((bytes / sizeof(wchar_t)) + 1, L'\0');
    status = RegGetValueW(HKEY_CURRENT_USER, keyPath, valueName.c_str(), RRF_RT_REG_SZ,
                          nullptr, buffer.data(), &bytes);
    if (status != ERROR_SUCCESS) {
        return L"";
    }

    return buffer.data();
}

// Writes a single REG_SZ string value, creating the key if necessary.
bool WriteStringValue(const wchar_t* keyPath, const std::wstring& valueName, const std::wstring& value) {
    HKEY hKey = nullptr;
    const LONG createStatus = RegCreateKeyExW(HKEY_CURRENT_USER, keyPath, 0, nullptr, 0,
                                              KEY_READ | KEY_WRITE, nullptr, &hKey, nullptr);
    if (createStatus != ERROR_SUCCESS) {
        return false;
    }

    const DWORD bytes  = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LONG  status = RegSetValueExW(hKey, valueName.c_str(), 0, REG_SZ,
                                        reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    RegCloseKey(hKey);
    return status == ERROR_SUCCESS;
}

// Loads and parses the IDR_DEFAULT_TARGETS resource embedded in the executable.
// This is the hard-coded default target list shipped with the binary, used to
// seed the registry on first run.
std::vector<std::wstring> LoadSeedTargetsFromResource() {
    HRSRC hResource = FindResourceW(GetModuleHandleW(nullptr),
                                    MAKEINTRESOURCEW(IDR_DEFAULT_TARGETS), RT_RCDATA);
    if (!hResource) {
        return {};
    }

    HGLOBAL hLoaded = LoadResource(GetModuleHandleW(nullptr), hResource);
    if (!hLoaded) {
        return {};
    }

    const DWORD size = SizeofResource(GetModuleHandleW(nullptr), hResource);
    const void* raw  = LockResource(hLoaded);
    if (!raw || size == 0) {
        return {};
    }

    const std::wstring text = DecodeTextBytes(reinterpret_cast<const BYTE*>(raw), size);
    return ParseProcessListText(text);
}

// Formats a single history entry as a pipe-delimited string:
//   "2024-03-15 14:22:01 | KILL | MsMpEng | C:\...\MsMpEng.exe"
// The path field is omitted if fullPath is empty or whitespace-only.
std::wstring BuildHistoryEntry(const std::wstring& action,
                               const std::wstring& processName,
                               const std::wstring& fullPath) {
    SYSTEMTIME localTime{};
    GetLocalTime(&localTime);

    wchar_t timestamp[32]{};
    swprintf_s(timestamp, L"%04u-%02u-%02u %02u:%02u:%02u",
               localTime.wYear, localTime.wMonth, localTime.wDay,
               localTime.wHour, localTime.wMinute, localTime.wSecond);

    std::wstring entry = timestamp;
    entry += L" | ";
    entry += action;
    entry += L" | ";
    entry += StripExeExtension(processName);

    const std::wstring trimmedPath = TrimWhitespace(fullPath);
    if (!trimmedPath.empty()) {
        entry += L" | ";
        entry += trimmedPath;
    }

    return entry;
}

// Strips .exe from every entry so the stored list is always bare names.
std::vector<std::wstring> NormalizeTargets(const std::vector<std::wstring>& processes) {
    std::vector<std::wstring> normalized;
    normalized.reserve(processes.size());

    for (const auto& process : processes) {
        const std::wstring stripped = StripExeExtension(process);
        if (!stripped.empty()) {
            normalized.push_back(stripped);
        }
    }

    return normalized;
}

} // namespace

// ===========================================================================
// Public API
// ===========================================================================

// Returns the built-in seed list without touching the registry.
// Used by ProcessKiller::GetDefaultTargetList() and for first-run seeding.
std::vector<std::wstring> ConfigReader::ReadSeedProcessList() {
    return LoadSeedTargetsFromResource();
}

// Returns the target process list.
// If the registry key is empty (first run), seeds it from the embedded resource
// and writes the seed back so subsequent reads are fast registry-only reads.
std::vector<std::wstring> ConfigReader::ReadProcessList() {
    std::lock_guard<std::mutex> lock(g_registryMutex);
    if (!EnsureRegistryScaffold()) {
        return {};
    }

    auto processes = ReadMultiStringValue(kTargetsKey, kTargetsValue);
    if (!processes.empty()) {
        return NormalizeTargets(processes);
    }

    // First-run seed: write the embedded defaults to the registry.
    processes = LoadSeedTargetsFromResource();
    if (!processes.empty()) {
        WriteMultiStringValue(kTargetsKey, kTargetsValue, processes);
    }
    return processes;
}

// Overwrites the stored target list.  Names are normalised (extension stripped)
// before writing.
bool ConfigReader::WriteProcessList(const std::vector<std::wstring>& processes) {
    std::lock_guard<std::mutex> lock(g_registryMutex);
    if (!EnsureRegistryScaffold()) {
        return false;
    }

    return WriteMultiStringValue(kTargetsKey, kTargetsValue, NormalizeTargets(processes));
}

// Returns the last-known full executable path for a given process name.
// processName may be with or without .exe — NormalizePathValueName handles both.
std::wstring ConfigReader::ReadProcessPath(const std::wstring& processName) {
    std::lock_guard<std::mutex> lock(g_registryMutex);
    if (!EnsureRegistryScaffold()) {
        return L"";
    }

    const std::wstring valueName = NormalizePathValueName(processName);
    if (valueName.empty()) {
        return L"";
    }

    return ReadStringValue(kPathsKey, valueName);
}

// Enumerates all entries in the Paths key and returns them as a vector.
// Used by ListViewManager to build the "previously seen non-built-in" rows.
std::vector<ProcessPathEntry> ConfigReader::ReadKnownProcessPaths() {
    std::lock_guard<std::mutex> lock(g_registryMutex);
    if (!EnsureRegistryScaffold()) {
        return {};
    }

    HKEY hKey = nullptr;
    const LONG openStatus = RegOpenKeyExW(HKEY_CURRENT_USER, kPathsKey, 0, KEY_READ, &hKey);
    if (openStatus != ERROR_SUCCESS) {
        return {};
    }

    // Query key metadata first so we can pre-size our scratch buffers.
    DWORD valueCount       = 0;
    DWORD maxValueNameLen  = 0;
    DWORD maxValueLen      = 0;
    RegQueryInfoKeyW(hKey, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                     &valueCount, &maxValueNameLen, &maxValueLen, nullptr, nullptr);

    std::vector<ProcessPathEntry> entries;
    std::vector<wchar_t> valueNameBuffer(maxValueNameLen + 2, L'\0');
    std::vector<BYTE>    valueDataBuffer(maxValueLen + sizeof(wchar_t), 0);

    for (DWORD index = 0; index < valueCount; ++index) {
        DWORD valueNameLen = maxValueNameLen + 1;
        DWORD valueDataLen = maxValueLen;
        DWORD type         = 0;

        const LONG enumStatus = RegEnumValueW(
            hKey, index,
            valueNameBuffer.data(), &valueNameLen,
            nullptr, &type,
            valueDataBuffer.data(), &valueDataLen);

        // Skip malformed entries: wrong type, empty name, or empty value.
        if (enumStatus != ERROR_SUCCESS || type != REG_SZ ||
            valueNameLen == 0 || valueDataLen < sizeof(wchar_t)) {
            continue;
        }

        const std::wstring processName = StripExeExtension(
            std::wstring(valueNameBuffer.data(), valueNameLen));
        const std::wstring fullPath    = TrimWhitespace(
            reinterpret_cast<const wchar_t*>(valueDataBuffer.data()));

        if (!processName.empty() && !fullPath.empty()) {
            entries.push_back({ processName, fullPath });
        }
    }

    RegCloseKey(hKey);
    return entries;
}

// Stores or updates the full path for a process.
// Called by FindProcesses every time a process is seen running, so the stored
// path stays current after reinstalls or moves.
bool ConfigReader::WriteProcessPath(const std::wstring& processName, const std::wstring& fullPath) {
    const std::wstring valueName  = NormalizePathValueName(processName);
    const std::wstring trimmedPath = TrimWhitespace(fullPath);
    if (valueName.empty() || trimmedPath.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_registryMutex);
    if (!EnsureRegistryScaffold()) {
        return false;
    }

    return WriteStringValue(kPathsKey, valueName, trimmedPath);
}

// Prepends a new entry to the history ring buffer, capping at kMaxHistoryEntries.
// Old entries are dropped from the tail.  An empty action or process name is
// rejected so the log stays meaningful.
bool ConfigReader::RecordHistory(const std::wstring& action,
                                 const std::wstring& processName,
                                 const std::wstring& fullPath) {
    const std::wstring trimmedAction = TrimWhitespace(action);
    const std::wstring trimmedName   = StripExeExtension(processName);
    if (trimmedAction.empty() || trimmedName.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_registryMutex);
    if (!EnsureRegistryScaffold()) {
        return false;
    }

    auto entries = ReadMultiStringValue(kHistoryKey, kHistoryValue);
    // Insert at front so the most recent entry is always entries[0].
    entries.insert(entries.begin(), BuildHistoryEntry(trimmedAction, trimmedName, fullPath));
    if (entries.size() > kMaxHistoryEntries) {
        entries.resize(kMaxHistoryEntries);
    }

    return WriteMultiStringValue(kHistoryKey, kHistoryValue, entries);
}
