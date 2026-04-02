// ProcessOperations.h
// Driver initialisation, kill/restore worker threads, and GUI action triggers.
// © WESMAR Marek Wesołowski

#pragma once

#include <windows.h>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Paralyze helpers (IFEO registry technique)
// ---------------------------------------------------------------------------

// Applies the IFEO Debugger block to a list of process names in a single batch.
// Returns the number of processes successfully paralyzed.
int ParalyzeProcessesBatch(const std::vector<std::wstring>& processNames);

// Removes the IFEO Debugger block from a list of process names in a single batch.
// Returns the number of processes successfully unparalyzed.
int UnparalyzeProcessesBatch(const std::vector<std::wstring>& processNames);

// Checks if a specific process is currently paralyzed (Debugger is set).
bool IsProcessParalyzed(const std::wstring& processName);

// ---------------------------------------------------------------------------
// Driver lifecycle
// ---------------------------------------------------------------------------

/// Loads and starts the kernel driver if it is not already active.
/// Returns true on success; false if any step fails.
bool InitializeDriver();

// ---------------------------------------------------------------------------
// Worker-thread entry points  (CreateThread targets — not called directly)
// ---------------------------------------------------------------------------

DWORD WINAPI KillOnceThreadProc(LPVOID lpParam);
DWORD WINAPI RestoreThreadProc(LPVOID lpParam);

// ---------------------------------------------------------------------------
// GUI action triggers  (called from WM_COMMAND on the UI thread)
// ---------------------------------------------------------------------------

/// Validates admin rights, loads the driver, and launches KillOnceThreadProc.
void KillOnceAction();

/// Validates admin rights and launches RestoreThreadProc.
void RestoreAction();
