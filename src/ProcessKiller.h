// ProcessKiller.h
// Process discovery, termination via driver IOCTL, and service restoration.
//
// KillProcess() sends an IOCTL to the loaded kernel driver which terminates
// the target process from kernel mode, bypassing user-space protection
// mechanisms.  RestoreProcess() / RestoreProcesses() restart previously
// terminated processes by locating their executables through the registry
// path cache or a live SCM query and launching them with appropriate
// privileges.
//
// © WESMAR Marek Wesołowski

#pragma once

#include <windows.h>
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

/// Describes a single process instance found in the system snapshot.
struct ProcessInfo {
    DWORD        pid;      ///< Process identifier; 0 when the process is not running.
    std::wstring name;     ///< Base executable name as reported by the snapshot (e.g. "svchost.exe").
    bool         running;  ///< True when a live instance with this PID was found.
};

/// Cached SCM entry used to avoid redundant service-database queries during
/// batch restore operations.
struct ServiceCacheEntry {
    std::wstring serviceName;  ///< SCM service name.
    std::wstring binaryPath;   ///< ImagePath value from the service configuration.
};

// ---------------------------------------------------------------------------
// ProcessKiller
// ---------------------------------------------------------------------------

/// Static helper class for process enumeration, kernel-mode termination, and
/// service-based restoration.  All methods are stateless and thread-safe
/// (they do not modify shared mutable state themselves; callers are
/// responsible for serialising access to the driver handle).
class ProcessKiller {
public:
    // -----------------------------------------------------------------------
    // Discovery
    // -----------------------------------------------------------------------

    /// Enumerates all running processes and returns an entry for each name in
    /// \p processNames.  Entries whose process is not currently running are
    /// included with pid = 0 and running = false.
    static std::vector<ProcessInfo> FindProcesses(
        const std::vector<std::wstring>& processNames);

    /// Returns true when a process with the given \p pid still exists in the
    /// system process table (i.e. has not yet fully exited).
    static bool IsProcessRunning(DWORD pid);

    // -----------------------------------------------------------------------
    // Termination
    // -----------------------------------------------------------------------

    /// Terminates the process identified by \p pid by dispatching a kill
    /// IOCTL through the open driver handle \p hDriver.
    /// Returns true when the IOCTL was accepted by the driver.
    static bool KillProcess(HANDLE hDriver, DWORD pid);

    /// Sends the raw kill IOCTL to the driver for \p pid.
    /// Separated from KillProcess() to allow unit-testing the IOCTL path
    /// without the higher-level retry logic.
    /// Returns true when DeviceIoControl succeeds.
    static bool SendIOCTL(HANDLE hDriver, DWORD pid);

    // -----------------------------------------------------------------------
    // Restoration
    // -----------------------------------------------------------------------

    /// Restarts the service or process associated with \p exeName.
    /// Looks up the executable path via ConfigReader, then launches it with
    /// the appropriate method (SCM start, ShellExecuteEx, or direct CreateProcess).
    /// Returns 0 on success, a non-zero error code on failure.
    /// Intended for single-process CLI restore calls.
    static int RestoreProcess(const std::wstring& exeName);

    /// Batch variant of RestoreProcess() optimised for GUI use: resolves paths
    /// and launches all processes in \p exeNames concurrently where possible,
    /// writing per-process result codes into the parallel \p results vector.
    /// \p results is resized to match \p exeNames before the operation begins.
    static void RestoreProcesses(const std::vector<std::wstring>& exeNames,
                                  std::vector<int>& results);

    // -----------------------------------------------------------------------
    // Default target list
    // -----------------------------------------------------------------------

    /// Returns the factory-default list of target process names (identical to
    /// the seed list compiled into the resource).  Used by the CLI "--list"
    /// command and first-run initialisation.
    static std::vector<std::wstring> GetDefaultTargetList();
};
