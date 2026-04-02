// DriverController.h
// Kernel driver management for netadapter.sys.
//
// Provides a static interface for the full driver lifecycle: registration,
// loading, IOCTL communication, unloading, and registry cleanup.  All public
// methods are safe to call from the UI thread; internally they may call
// SCM (Service Control Manager) and kernel APIs that require administrator
// privileges and, for operations in protected directories, TrustedInstaller
// impersonation.
//
// © WESMAR Marek Wesołowski

#pragma once

#include <windows.h>
#include <string>
#include <vector>

class DriverController {
public:
    // -----------------------------------------------------------------------
    // Driver presence checks
    // -----------------------------------------------------------------------

    /// Returns true when the application's own driver (netadapter.sys) is
    /// currently registered with the SCM and in the Running state.
    /// Distinguished from generic device existence checks.
    static bool IsDriverLoaded();

    /// Returns true when the driver identified by SERVICE_NAME is both
    /// registered and verified to be the application's own copy (not a
    /// third-party service with the same name).
    static bool IsOurDriverLoaded();

    // -----------------------------------------------------------------------
    // Driver lifecycle
    // -----------------------------------------------------------------------

    /// Registers netadapter.sys as an SCM service using the given driver image
    /// path.  On failure at any intermediate step the registration is rolled
    /// back automatically so the SCM is not left in an inconsistent state.
    /// Returns true on success.
    static bool RegisterDriver(const std::wstring& driverPath);

    /// Starts the previously registered driver service.
    /// Performs an atomic start sequence; any failure triggers a rollback.
    /// Returns true on success.
    static bool StartDriver();

    /// Stops the running driver service.
    /// Returns true on success or when the driver is already stopped.
    static bool StopDriver();

    /// Unregisters the driver from the SCM.
    /// Should be called only after StopDriver() has returned true.
    /// Returns true on success.
    static bool UnregisterDriver();

    /// High-level helper: registers, starts, and validates the driver in a
    /// single call.  Intended for callers that do not manage the lifecycle
    /// step-by-step.  Returns true when the driver is active on return.
    static bool LoadDriverTemporarily();

    /// Stops and unregisters the driver, then calls CleanupDriverRegistry()
    /// to erase any registry artefacts left by the session.
    static void UnloadDriver();

    // -----------------------------------------------------------------------
    // Driver handle
    // -----------------------------------------------------------------------

    /// Opens and returns a handle to the driver's device node (DEVICE_NAME).
    /// The handle is cached internally; subsequent calls return the same handle
    /// without re-opening the device.
    static HANDLE GetDriverHandle();

    /// Closes the cached driver device handle.
    /// Must be called before UnloadDriver() to avoid handle leaks.
    static void CloseDriverHandle();

    // -----------------------------------------------------------------------
    // Miscellaneous
    // -----------------------------------------------------------------------

    /// Returns the resolved absolute path of the driver image (netadapter.sys)
    /// as it was registered with the SCM, or an empty string if not registered.
    static std::wstring GetDriverPath();

    /// Removes all SCM and registry keys created during driver registration.
    /// Called automatically by UnloadDriver(); may also be called standalone
    /// after a crash recovery to leave a clean system state.
    static void CleanupDriverRegistry();

private:
    /// Cached handle to the driver's kernel device object.
    static HANDLE hDriver;

    static constexpr const wchar_t* SERVICE_NAME         = L"netadapter";
    static constexpr const wchar_t* DEVICE_NAME          = L"\\\\.\\Warsaw_PM";
    static constexpr const wchar_t* SERVICE_REGISTRY_PATH =
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\netadapter";

    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

    /// Impersonates the TrustedInstaller account to allow file-system and
    /// registry operations in protected Windows directories.
    /// Returns true on success; the caller must revert impersonation.
    static bool ImpersonateTrustedInstaller();

    /// Locates the DriverStore path where the driver package was originally
    /// staged by Windows, if applicable.
    static std::wstring FindDriverStorePath();

    /// Extracts the driver binary that is embedded inside the application's
    /// icon resource and writes it to \p targetPath.
    /// Returns true on success.
    static bool ExtractDriverFromIcon(const std::wstring& targetPath);

    /// Enables a named privilege (e.g. SeLoadDriverPrivilege) in the current
    /// process token.  Returns true when the privilege was granted.
    static bool EnablePrivilege(LPCWSTR privilege);

    /// Verifies that the loaded driver binary matches the application's own
    /// copy (e.g. by version resource or embedded checksum).
    /// Returns true when ownership is confirmed.
    static bool VerifyDriverOwnership();
};
