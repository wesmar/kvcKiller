// GlobalData.h
// Shared global variables, handles, and application-wide constants.
//
// All globals declared here are defined in GlobalData.cpp.  Every other
// translation unit that needs them must include this header and link against
// GlobalData.obj — do not re-define them elsewhere.
//
// © WESMAR Marek Wesołowski

#pragma once

#include <windows.h>
#include <commctrl.h>
#include <atomic>
#include <string>
#include <map>

// =============================================================================
// Window / GDI handles
// =============================================================================

extern HINSTANCE hInst;                   ///< Module instance handle (set in WinMain).
extern HWND      hInstMain;               ///< Top-level application window.
extern HWND      hHeaderInfoText;         ///< Informational header label.
extern HWND      hHeaderWarningText;      ///< Warning header label.
extern HWND      hListView;               ///< Main process ListView control.
extern HWND      hShowExtraCheck;         ///< "Show extra processes" checkbox.
extern HWND      hHideInactiveBuiltInCheck; ///< "Hide inactive built-in" checkbox.
extern HWND      hParalyzeCheck;          ///< "Paralyze" mode checkbox.
extern HWND      hParalyzeLabel;          ///< Descriptive label for the paralyze checkbox.
extern HWND      hKillOnceButton;         ///< "Kill" action button.
extern HWND      hRestoreButton;          ///< "Restore" action button.
extern HWND      hStatusText;             ///< Status bar text label.
extern HWND      hProcessCount;           ///< Footer label showing process counts.
extern HIMAGELIST hImageList;             ///< Image list shared by the ListView.

extern HFONT   hUiFont;          ///< Primary UI font used for most controls.
extern HFONT   hButtonFont;      ///< Font applied to action buttons.
extern HFONT   hStatusFont;      ///< Font applied to the status bar.
extern HFONT   hCountFont;       ///< Font applied to the process-count footer.
extern HBRUSH  hWindowBrush;     ///< Background brush for the main window.
extern bool    g_isDarkMode;     ///< True when the system dark-mode theme is active.
extern COLORREF g_windowColor;  ///< Resolved window background colour for the current theme.

// =============================================================================
// Window dimensions
// =============================================================================

constexpr int WINDOW_WIDTH      = 860;   ///< Default window width in logical pixels.
constexpr int WINDOW_HEIGHT     = 660;   ///< Default window height in logical pixels.
constexpr int MIN_WINDOW_WIDTH  = 760;   ///< Minimum resizable width.
constexpr int MIN_WINDOW_HEIGHT = 620;   ///< Minimum resizable height.
constexpr int BUTTON_WIDTH      = 170;   ///< Width of each action button.
constexpr int BUTTON_HEIGHT     = 40;    ///< Height of each action button.

// =============================================================================
// Column width configuration
//
// Fixed-width columns (Source, PID, Status, Last Action) do not change when
// the window is resized.  The "Process Name" column (index 0) expands to fill
// all remaining horizontal space.  Adjust the constants below to change the
// widths of the fixed columns.
// =============================================================================

constexpr int kColSourceCx     =  60;   ///< "Source"      — fits "SOURCE" / "Built-in".
constexpr int kColPidCx        =  68;   ///< "PID"         — fits a 6-digit PID.
constexpr int kColStatusCx     = 118;   ///< "Status"      — fits "NOT RUNNING".
constexpr int kColLastActionCx = 120;   ///< "Last Action" — fits "RESTARTED".
constexpr int kColNameMinCx    = 140;   ///< Minimum width for the "Process Name" column.

// =============================================================================
// ListView row flag bits  (stored in LVITEM::lParam)
// =============================================================================

constexpr LPARAM kRowFlagRunning    = 0x0001;  ///< Process is currently running.
constexpr LPARAM kRowFlagExtra      = 0x0002;  ///< Row is an "extra" (not in config).
constexpr LPARAM kRowFlagKilled     = 0x0004;  ///< Process was killed this session.
constexpr LPARAM kRowFlagRestored   = 0x0008;  ///< Process was restored this session.
constexpr LPARAM kRowFlagRestarted  = 0x0010;  ///< Process restarted on its own after being killed.
constexpr LPARAM kRowFlagParalyzed  = 0x0020;  ///< Process is killed AND has IFEO Debugger entry blocking restart.

// =============================================================================
// Action / cell string constants
//
// Used both as state-comparison keys and as the display text in the ListView.
// Centralised here so every module uses identical string literals.
// =============================================================================

constexpr wchar_t kActionNone[]       = L"-";
constexpr wchar_t kActionKilled[]     = L"KILLED";
constexpr wchar_t kActionParalyzed[]  = L"PARALYZED";
constexpr wchar_t kActionRestored[]   = L"RESTORED";
constexpr wchar_t kActionRestarted[]  = L"RESTARTED";
constexpr wchar_t kActionRestoring[]  = L"RESTORING";
constexpr wchar_t kActionLive[]       = L"LIVE";
constexpr wchar_t kSourceReg[]        = L"Built-in";
constexpr wchar_t kSourceSaved[]      = L"Saved";
constexpr wchar_t kStatusRunning[]    = L"RUNNING";
constexpr wchar_t kStatusNotRunning[] = L"NOT RUNNING";

// =============================================================================
// Sort state
// =============================================================================

constexpr int kSortColumnNatural = -1;  ///< Sentinel: no explicit sort column active (natural order).

extern int  g_sortColumn;     ///< Index of the column currently used for sorting, or kSortColumnNatural.
extern bool g_sortAscending;  ///< True when the active sort direction is ascending.

// =============================================================================
// Thread-safe per-process state
// =============================================================================

extern CRITICAL_SECTION                      g_statusLock;         ///< Guards g_lastActionByName and g_prevRunningByName.
extern std::map<std::wstring, std::wstring>  g_lastActionByName;   ///< Maps normalised process key -> last action string.
extern std::map<std::wstring, bool>          g_prevRunningByName;  ///< Maps normalised process key -> running state from the previous refresh cycle.

// =============================================================================
// WM_USER message IDs
//
// Named constants prevent the "WM_USER + 1 / + 2 / + 3" magic-number pattern
// from spreading across multiple translation units.
// =============================================================================

constexpr UINT WMU_REFRESH_LIST = WM_USER + 1;  ///< Requests a full ListView rebuild.  No parameters.
constexpr UINT WMU_SET_BUTTONS  = WM_USER + 2;  ///< wParam: 1 = enable action buttons, 0 = disable.
constexpr UINT WMU_STATUS_TEXT  = WM_USER + 3;  ///< wParam: heap-allocated std::wstring*; handler deletes it.

// =============================================================================
// Shutdown coordination
//
// g_shutdownPending is set in WM_CLOSE before the window is destroyed.
// Worker threads check it before posting any cross-thread message, allowing
// them to exit cleanly without touching destroyed UI state.
// g_activeWorkers is incremented before CreateThread and decremented by the
// WorkerGuard RAII wrapper at thread exit, so WM_CLOSE can spin-wait until
// all workers have finished.
// =============================================================================

extern std::atomic<bool> g_shutdownPending;  ///< Set to true when the application is shutting down.
extern std::atomic<int>  g_activeWorkers;    ///< Number of worker threads currently running.
