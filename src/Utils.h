// Utils.h
// Pure utility functions: string helpers, OS detection, admin check,
// thread-safe per-process state accessors, and cross-thread helpers.
// No GUI or driver headers are required by callers.
// © WESMAR Marek Wesołowski

#pragma once

#include <windows.h>
#include <string>

// ---------------------------------------------------------------------------
// String resources
// ---------------------------------------------------------------------------

/// Load a string resource by ID from the current module.
std::wstring LoadStr(UINT id);

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
std::wstring ToLowerCopy(std::wstring value);

/// Strips a trailing ".exe" suffix (case-insensitive).
std::wstring StripExeExtension(const std::wstring& value);

/// Ensures the name ends with ".exe" (strips and re-appends).
std::wstring EnsureExeName(const std::wstring& value);

/// Returns the canonical map key: lower-case name without ".exe".
std::wstring NormalizeProcessKey(const std::wstring& value);

/// Formats a PID as a string; returns L"-" for PID 0.
std::wstring FormatPid(DWORD pid);

// ---------------------------------------------------------------------------
// Thread-safe per-process state (backed by globals in GlobalData)
// ---------------------------------------------------------------------------

void         StoreLastAction(const std::wstring& processName, const std::wstring& action);
std::wstring ReadLastAction(const std::wstring& processName);

bool WasRunningPreviously(const std::wstring& processName);
void StoreRunningState(const std::wstring& processName, bool running);

// ---------------------------------------------------------------------------
// Cross-thread UI helpers
// ---------------------------------------------------------------------------

/// Posts WMU_SET_BUTTONS to the main window from any thread.
void SetButtonsEnabled(bool enabled);
