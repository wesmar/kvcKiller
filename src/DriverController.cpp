#include "DriverController.h"
#include "DriverPayload.h"

#include <algorithm>
#include <cstring>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace {

enum DriverIndex {
    kPplUnprotector = 0,
    kProcessTerminator = 1,
    kDriverCount = 2
};

struct DriverSpec {
    const wchar_t* cabName;
    const wchar_t* serviceName;
    const wchar_t* displayName;
    const wchar_t* devicePath;
    const wchar_t* diskName;
    bool configureRegistry;
};

constexpr DriverSpec kDrivers[kDriverCount] = {
    {
        L"driver1",
        L"KvcPplUnprotect",
        L"KVC PPL unprotect component",
        L"\\\\.\\RTCore64",
        L"kvckiller1",
        false
    },
    {
        L"driver2",
        L"KvcProcessKill",
        L"KVC process termination component",
        L"\\\\.\\DosK7RKScnDrv",
        L"kvckiller2",
        true
    }
};

constexpr wchar_t kK7RegistryPath[] =
    L"SOFTWARE\\Wow6432Node\\K7 Computing\\K7TotalSecurity";
constexpr wchar_t kK7ParentPath[] =
    L"SOFTWARE\\Wow6432Node\\K7 Computing";
constexpr wchar_t kK7ValueName[] = L"DirProductBase";

struct DriverState {
    HANDLE device = INVALID_HANDLE_VALUE;
};

DriverState g_driverState[kDriverCount];

struct RegistryState {
    bool initialized = false;
    bool keyExisted = false;
    bool parentExisted = false;
    bool valueExisted = false;
    DWORD valueType = REG_NONE;
    std::vector<BYTE> valueData;
};

RegistryState g_k7Registry;

int IndexOf(DriverRole role) {
    return role == DriverRole::PplUnprotector ? kPplUnprotector : kProcessTerminator;
}

const DriverSpec& SpecOf(DriverRole role) {
    return kDrivers[IndexOf(role)];
}

bool QuerySystemDirectory(std::wstring& path) {
    wchar_t buffer[MAX_PATH];
    const UINT length = GetSystemDirectoryW(buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;
    path.assign(buffer, length);
    return true;
}

bool GetExecutableDirectory(std::wstring& path) {
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;

    wchar_t* slash = wcsrchr(buffer, L'\\');
    if (!slash) return false;
    *slash = L'\0';
    path.assign(buffer);
    return true;
}

bool ParentKeyExists(const wchar_t* path) {
    HKEY key = nullptr;
    const LONG result = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        path,
        0,
        KEY_READ | KEY_WOW64_64KEY,
        &key
    );
    if (result == ERROR_SUCCESS) {
        RegCloseKey(key);
        return true;
    }
    return false;
}

bool KeyIsEmpty(const wchar_t* path) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0,
                      KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return true;
    }

    DWORD subkeys = 0;
    DWORD values = 0;
    const LONG result = RegQueryInfoKeyW(
        key,
        nullptr, nullptr, nullptr,
        &subkeys,
        nullptr, nullptr,
        &values,
        nullptr, nullptr, nullptr, nullptr
    );
    RegCloseKey(key);
    return result == ERROR_SUCCESS && subkeys == 0 && values == 0;
}

bool FileMatches(const std::wstring& path, const std::vector<BYTE>& expected) {
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    const bool sizeMatches = GetFileSizeEx(file, &size)
        && size.QuadPart == static_cast<LONGLONG>(expected.size());

    std::vector<BYTE> actual;
    bool contentMatches = false;
    if (sizeMatches) {
        actual.resize(expected.size());
        DWORD total = 0;
        while (total < expected.size()) {
            const DWORD request = static_cast<DWORD>(
                (std::min)(static_cast<size_t>(4096), expected.size() - total)
            );
            DWORD read = 0;
            if (!ReadFile(file, actual.data() + total, request, &read, nullptr)
                || read != request) {
                break;
            }
            total += read;
        }
        contentMatches = total == expected.size()
            && std::memcmp(actual.data(), expected.data(), expected.size()) == 0;
    }

    CloseHandle(file);
    return contentMatches;
}

bool WriteDriverFile(const std::wstring& path, const std::vector<BYTE>& data) {
    if (data.empty()) return false;
    if (FileMatches(path, data)) return true;

    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(path.c_str());

    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (file == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    const DWORD size = static_cast<DWORD>(data.size());
    const BOOL success = WriteFile(file, data.data(), size, &written, nullptr);
    CloseHandle(file);

    if (!success || written != size) {
        DeleteFileW(path.c_str());
        return false;
    }

    return FileMatches(path, data);
}

bool SaveK7RegistryState() {
    if (g_k7Registry.initialized) return true;

    g_k7Registry.parentExisted = ParentKeyExists(kK7ParentPath);

    HKEY key = nullptr;
    const LONG openResult = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        kK7RegistryPath,
        0,
        KEY_QUERY_VALUE | KEY_WOW64_64KEY,
        &key
    );
    g_k7Registry.keyExisted = openResult == ERROR_SUCCESS;

    if (g_k7Registry.keyExisted) {
        DWORD type = REG_NONE;
        DWORD size = 0;
        LONG result = RegQueryValueExW(
            key, kK7ValueName, nullptr, &type, nullptr, &size
        );
        if (result == ERROR_SUCCESS) {
            g_k7Registry.valueExisted = true;
            g_k7Registry.valueType = type;
            g_k7Registry.valueData.resize(size);
            if (size > 0) {
                result = RegQueryValueExW(
                    key, kK7ValueName, nullptr, &type,
                    g_k7Registry.valueData.data(), &size
                );
                if (result != ERROR_SUCCESS) {
                    RegCloseKey(key);
                    g_k7Registry = {};
                    return false;
                }
                g_k7Registry.valueData.resize(size);
            }
        }
        RegCloseKey(key);
    }

    g_k7Registry.initialized = true;
    return true;
}

bool ConfigureK7Registry() {
    if (!SaveK7RegistryState()) return false;

    std::wstring executableDirectory;
    if (!GetExecutableDirectory(executableDirectory)) return false;

    HKEY key = nullptr;
    const LONG createResult = RegCreateKeyExW(
        HKEY_LOCAL_MACHINE,
        kK7RegistryPath,
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE | KEY_QUERY_VALUE | KEY_WOW64_64KEY,
        nullptr,
        &key,
        nullptr
    );
    if (createResult != ERROR_SUCCESS) return false;

    const DWORD bytes = static_cast<DWORD>(
        (executableDirectory.size() + 1) * sizeof(wchar_t)
    );
    LONG result = RegSetValueExW(
        key,
        kK7ValueName,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(executableDirectory.c_str()),
        bytes
    );
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) return false;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kK7RegistryPath, 0,
                      KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return false;
    }

    wchar_t value[MAX_PATH]{};
    DWORD size = sizeof(value);
    DWORD type = REG_NONE;
    result = RegQueryValueExW(
        key, kK7ValueName, nullptr, &type,
        reinterpret_cast<BYTE*>(value), &size
    );
    RegCloseKey(key);

    return result == ERROR_SUCCESS
        && type == REG_SZ
        && _wcsicmp(value, executableDirectory.c_str()) == 0;
}

void RestoreK7Registry() {
    if (!g_k7Registry.initialized) return;

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kK7RegistryPath, 0,
                      KEY_SET_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
        if (g_k7Registry.valueExisted) {
            RegSetValueExW(
                key,
                kK7ValueName,
                0,
                g_k7Registry.valueType,
                g_k7Registry.valueData.empty()
                    ? nullptr
                    : g_k7Registry.valueData.data(),
                static_cast<DWORD>(g_k7Registry.valueData.size())
            );
        } else {
            RegDeleteValueW(key, kK7ValueName);
        }
        RegCloseKey(key);
    }

    if (!g_k7Registry.keyExisted && KeyIsEmpty(kK7RegistryPath)) {
        RegDeleteKeyExW(HKEY_LOCAL_MACHINE, kK7RegistryPath, KEY_WOW64_64KEY, 0);
    }
    if (!g_k7Registry.parentExisted && KeyIsEmpty(kK7ParentPath)) {
        RegDeleteKeyExW(HKEY_LOCAL_MACHINE, kK7ParentPath, KEY_WOW64_64KEY, 0);
    }

    g_k7Registry = {};
}

void WaitForServiceState(SC_HANDLE service, DWORD desiredState, DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    SERVICE_STATUS status{};
    while (GetTickCount64() < deadline) {
        if (!QueryServiceStatus(service, &status)) return;
        if (status.dwCurrentState == desiredState) return;
        Sleep(50);
    }
}

void RemoveDriverFile(DriverRole role) {
    const std::wstring path = DriverController::GetDriverPath(role);
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!DeleteFileW(path.c_str())) {
        MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
}

} // namespace

bool DriverController::EnablePrivilege(LPCWSTR privilege) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                          &token)) {
        return false;
    }

    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, privilege, &luid)) {
        CloseHandle(token);
        return false;
    }

    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    SetLastError(ERROR_SUCCESS);
    const BOOL adjusted = AdjustTokenPrivileges(
        token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr
    );
    const DWORD error = GetLastError();
    CloseHandle(token);
    return adjusted && error == ERROR_SUCCESS;
}

std::wstring DriverController::GetDriverPath(DriverRole role) {
    std::wstring systemDirectory;
    if (!QuerySystemDirectory(systemDirectory)) return {};
    return systemDirectory + L"\\drivers\\" + SpecOf(role).diskName;
}

bool DriverController::ExtractDriverFile(DriverRole role) {
    std::vector<DriverPayloadFile> payloads;
    if (!ExtractDriverPayloads(payloads)) return false;

    const DriverSpec& spec = SpecOf(role);
    std::string expectedName;
    for (const wchar_t* p = spec.cabName; *p; ++p) {
        expectedName.push_back(static_cast<char>(*p));
    }

    for (const DriverPayloadFile& payload : payloads) {
        if (_stricmp(payload.name.c_str(), expectedName.c_str()) == 0) {
            return WriteDriverFile(GetDriverPath(role), payload.data);
        }
    }
    return false;
}

bool DriverController::ConfigureDriver(DriverRole role) {
    if (SpecOf(role).configureRegistry) {
        return ConfigureK7Registry();
    }
    return true;
}

void DriverController::RestoreDriverConfiguration(DriverRole role) {
    if (SpecOf(role).configureRegistry) {
        RestoreK7Registry();
    }
}

HANDLE DriverController::LoadDriver(DriverRole role) {
    const int index = IndexOf(role);
    DriverState& state = g_driverState[index];
    if (state.device != INVALID_HANDLE_VALUE) return state.device;

    EnablePrivilege(SE_LOAD_DRIVER_NAME);
    EnablePrivilege(SE_DEBUG_NAME);

    const std::wstring driverPath = GetDriverPath(role);
    if (driverPath.empty() || !ExtractDriverFile(role)) return INVALID_HANDLE_VALUE;
    if (!ConfigureDriver(role)) {
        RestoreDriverConfiguration(role);
        return INVALID_HANDLE_VALUE;
    }

    const DriverSpec& spec = SpecOf(role);
    SC_HANDLE scm = OpenSCManagerW(
        nullptr,
        nullptr,
        SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE
    );
    if (!scm) {
        RestoreDriverConfiguration(role);
        return INVALID_HANDLE_VALUE;
    }

    SC_HANDLE service = OpenServiceW(
        scm,
        spec.serviceName,
        SERVICE_START | SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS |
        SERVICE_CHANGE_CONFIG
    );

    if (!service) {
        service = CreateServiceW(
            scm,
            spec.serviceName,
            spec.displayName,
            SERVICE_START | SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS |
            SERVICE_CHANGE_CONFIG,
            SERVICE_KERNEL_DRIVER,
            SERVICE_DEMAND_START,
            SERVICE_ERROR_NORMAL,
            driverPath.c_str(),
            nullptr, nullptr, nullptr, nullptr, nullptr
        );
    }

    if (!service) {
        CloseServiceHandle(scm);
        RestoreDriverConfiguration(role);
        return INVALID_HANDLE_VALUE;
    }

    if (!ChangeServiceConfigW(
            service,
            SERVICE_NO_CHANGE,
            SERVICE_DEMAND_START,
            SERVICE_ERROR_NORMAL,
            driverPath.c_str(),
            nullptr, nullptr, nullptr, nullptr, nullptr,
            spec.displayName)) {
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        RestoreDriverConfiguration(role);
        return INVALID_HANDLE_VALUE;
    }

    if (!StartServiceW(service, 0, nullptr)) {
        const DWORD error = GetLastError();
        if (error != ERROR_SERVICE_ALREADY_RUNNING) {
            CloseServiceHandle(service);
            CloseServiceHandle(scm);
            RestoreDriverConfiguration(role);
            return INVALID_HANDLE_VALUE;
        }
    }

    WaitForServiceState(service, SERVICE_RUNNING, 5000);
    CloseServiceHandle(service);
    CloseServiceHandle(scm);

    state.device = CreateFileW(
        spec.devicePath,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );

    if (state.device == INVALID_HANDLE_VALUE) {
        StopAndRemoveService(role);
        RestoreDriverConfiguration(role);
    }

    return state.device;
}

HANDLE DriverController::GetDriverHandle(DriverRole role) {
    return g_driverState[IndexOf(role)].device;
}

bool DriverController::IsDriverLoaded(DriverRole role) {
    return g_driverState[IndexOf(role)].device != INVALID_HANDLE_VALUE;
}

void DriverController::StopAndRemoveService(DriverRole role) {
    const int index = IndexOf(role);
    DriverState& state = g_driverState[index];
    if (state.device != INVALID_HANDLE_VALUE) {
        CloseHandle(state.device);
        state.device = INVALID_HANDLE_VALUE;
    }

    SC_HANDLE scm = OpenSCManagerW(
        nullptr, nullptr, SC_MANAGER_CONNECT
    );
    if (!scm) return;

    SC_HANDLE service = OpenServiceW(
        scm,
        SpecOf(role).serviceName,
        SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS
    );
    if (service) {
        SERVICE_STATUS status{};
        ControlService(service, SERVICE_CONTROL_STOP, &status);
        WaitForServiceState(service, SERVICE_STOPPED, 5000);
        DeleteService(service);
        CloseServiceHandle(service);
    }

    CloseServiceHandle(scm);
}

void DriverController::CleanupAll() {
    StopAndRemoveService(DriverRole::ProcessTerminator);
    StopAndRemoveService(DriverRole::PplUnprotector);
    RestoreDriverConfiguration(DriverRole::ProcessTerminator);

    // Give the kernel a short window to release the image mapping before the
    // two extensionless files are removed from the drivers directory.
    Sleep(100);
    RemoveDriverFile(DriverRole::ProcessTerminator);
    RemoveDriverFile(DriverRole::PplUnprotector);
}
