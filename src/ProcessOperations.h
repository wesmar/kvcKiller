// ProcessOperations.h
// Driver initialisation, kill/restore worker threads, and GUI action triggers.
// © WESMAR Marek Wesołowski

#pragma once

#include <windows.h>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

inline constexpr std::array<std::wstring_view, 3> kDefenderTargets{
    L"MsMpEng",
    L"SecurityHealthService",
    L"SecurityHealthSystray"
};

std::vector<std::wstring> DefenderTargetList();

enum class KillOutcome {
    Succeeded,
    Partial,
    Failed,
    AlreadyStopped
};

struct KillTargetReport {
    std::wstring name;
    DWORD pid = 0;
    std::vector<DWORD> initialPids;
    bool wasRunning = false;
    bool trapSet = false;
    bool terminated = false;
    bool runningAfter = false;
    bool originalSurvived = false;
    bool restartedAfter = false;
};

struct KillOperationReport {
    bool pplReady = false;
    bool terminatorReady = false;
    bool winDefendStopped = false;
    bool securityHealthStopped = false;
    bool defenderSet = false;
    KillOutcome outcome = KillOutcome::Failed;
    std::vector<KillTargetReport> targets;
};

/// Shared CLI/GUI kill pipeline. It applies IFEO first when requested, lowers
/// protection for the running batch, terminates targets, verifies the final
/// state, and evaluates Defender by service state instead of helper processes.
KillOperationReport ExecuteKillTargets(
    std::span<const std::wstring> targets,
    bool paralyzeFirst,
    bool defenderSet);

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

/// Kills MsMpEng, SecurityHealthService, and SecurityHealthSystray as one
/// group. IFEO suppression is applied before the process kill phase.
void KillDefenderAction();

/// Validates admin rights and launches RestoreThreadProc.
void RestoreAction();
