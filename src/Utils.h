// Utils.h
// Pure utility functions: string helpers, OS detection, admin check,
// thread-safe per-process state accessors, and cross-thread helpers.
// No GUI or driver headers are required by callers.
// © WESMAR Marek Wesołowski

#pragma once

#include <windows.h>
#include "KvcContainers.h"

// ---------------------------------------------------------------------------
// String resources
// ---------------------------------------------------------------------------

/// Load a string resource by ID from the current module.
WString LoadStr(UINT id);

// ---------------------------------------------------------------------------
// OS / environment detection
// ---------------------------------------------------------------------------

/// Returns true when running on Windows 11 (build 22000) or newer.
bool IsWindows11OrLater();

/// Returns true when the current process token belongs to the Administrators group.
bool IsRunningAsAdmin();

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------

/// Returns a lower-case copy of \p value.
WString ToLowerCopy(WString value);

/// Strips a trailing ".exe" suffix (case-insensitive).
WString StripExeExtension(const WString& value);

/// Ensures the name ends with ".exe" (strips and re-appends).
WString EnsureExeName(const WString& value);

/// Returns the canonical map key: lower-case name without ".exe".
WString NormalizeProcessKey(const WString& value);

/// Formats a PID as a string; returns L"-" for PID 0.
WString FormatPid(DWORD pid);

// ---------------------------------------------------------------------------
// Thread-safe per-process state (backed by globals in GlobalData)
// ---------------------------------------------------------------------------

void    StoreLastAction(const WString& processName, const WString& action);
WString ReadLastAction(const WString& processName);

bool WasRunningPreviously(const WString& processName);
void StoreRunningState(const WString& processName, bool running);

// ---------------------------------------------------------------------------
// Cross-thread UI helpers
// ---------------------------------------------------------------------------

/// Posts WMU_SET_BUTTONS to the main window from any thread.
void SetButtonsEnabled(bool enabled);
