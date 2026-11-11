// ConfigReader.h
// Registry-backed configuration store with embedded default seed list.
//
// All persistent settings (target process names, known executable paths, and
// operation history) are stored under HKCU in the application's registry key.
// The seed list is compiled into the binary as a raw resource and is used to
// pre-populate the registry on the very first run.
//
// © WESMAR Marek Wesołowski

#pragma once

#include <windows.h>
#include "KvcContainers.h"

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

/// Associates a process name with the full path of its executable on disk.
/// Used when the path cannot be derived from a running instance and must be
/// read from the persistent path cache in the registry.
struct ProcessPathEntry {
    WString processName;  ///< Base name of the executable (e.g. "svchost.exe").
    WString fullPath;     ///< Absolute path to the executable on disk.
};

// ---------------------------------------------------------------------------
// ConfigReader
// ---------------------------------------------------------------------------

/// Provides static methods for reading and writing application configuration
/// from the Windows registry. All operations target the current user's hive
/// (HKCU) and are synchronised with an internal SRWLOCK.
class ConfigReader {
public:
    // -----------------------------------------------------------------------
    // Target process list
    // -----------------------------------------------------------------------

    /// Reads the user-editable list of target process names from the registry.
    /// Returns the persisted list, or the seed list when no persisted data exists.
    static Vector<WString> ReadProcessList();

    /// Overwrites the persisted process list with \p processes.
    /// Returns true on success, false if the registry write fails.
    static bool WriteProcessList(const Vector<WString>& processes);

    /// Loads the factory-default ("seed") process list that is compiled into
    /// the binary as a raw resource. Used to initialise the registry on first
    /// run and to offer a "reset to defaults" capability.
    static Vector<WString> ReadSeedProcessList();

    // -----------------------------------------------------------------------
    // Known executable paths
    // -----------------------------------------------------------------------

    /// Returns the full executable path associated with \p processName, or an
    /// empty string if no path has been recorded for that name.
    static WString ReadProcessPath(const WString& processName);

    /// Returns every name->path pair stored in the path cache.
    static Vector<ProcessPathEntry> ReadKnownProcessPaths();

    /// Stores or updates the executable path for \p processName in the registry.
    /// Returns true on success, false if the write fails.
    static bool WriteProcessPath(const WString& processName,
                                 const WString& fullPath);

    // -----------------------------------------------------------------------
    // Operation history
    // -----------------------------------------------------------------------

    /// Appends an audit-trail entry to the registry history log.
    /// \param action      Human-readable verb, e.g. "KILLED" or "RESTORED".
    /// \param processName Target process associated with the action.
    /// \param fullPath    Optional executable path; omitted when not relevant.
    /// Returns true on success, false if the registry write fails.
    static bool RecordHistory(const WString& action,
                              const WString& processName,
                              const WString& fullPath = WString());
};
