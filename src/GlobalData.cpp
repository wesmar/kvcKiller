// GlobalData.cpp
// Definitions of application-wide global variables declared in GlobalData.h.
// © WESMAR Marek Wesołowski
//
// All variables here are zero- or null-initialised at program start (static
// storage duration).  WM_CREATE in main.cpp sets the non-null initial values
// after the window and child controls have been created.
//
// Access rules:
//   - HWND / HFONT / HBRUSH handles: UI thread only (no locking needed).
//   - g_sortColumn / g_sortAscending: UI thread only.
//   - g_lastActionByName / g_prevRunningByName: protected by g_statusLock.
//   - g_shutdownPending / g_activeWorkers: std::atomic, any thread.

#include "GlobalData.h"

// ---------------------------------------------------------------------------
// Window and child-control handles
// ---------------------------------------------------------------------------

HINSTANCE  hInst                    = nullptr;  // Module instance (set in WinMain)
HWND       hInstMain                = nullptr;  // Main window — target for PostMessage from workers
HWND       hHeaderInfoText          = nullptr;  // Left header panel static text
HWND       hHeaderWarningText       = nullptr;  // Right header panel static text
HWND       hListView                = nullptr;  // Process list ListView
HWND       hShowExtraCheck          = nullptr;  // "Show extra processes" checkbox
HWND       hHideInactiveBuiltInCheck= nullptr;  // "Hide inactive built-in" checkbox
HWND       hParalyzeCheck           = nullptr;  // "Paralyze" bare checkbox square
HWND       hParalyzeLabel           = nullptr;  // SS_NOTIFY STATIC acting as the paralyze label
HWND       hKillOnceButton          = nullptr;  // Kill action button
HWND       hRestoreButton           = nullptr;  // Restore action button
HWND       hStatusText              = nullptr;  // Status line below the buttons
HWND       hProcessCount            = nullptr;  // "Targets: N | Running: M" info label
HIMAGELIST hImageList               = nullptr;  // Reserved for a future icon set

// ---------------------------------------------------------------------------
// GDI objects
// ---------------------------------------------------------------------------

HFONT    hUiFont      = nullptr;          // 9pt body font for most controls
HFONT    hButtonFont  = nullptr;          // 10pt semi-bold for action buttons
HFONT    hStatusFont  = nullptr;          // 12pt semi-bold for the status line
HFONT    hCountFont   = nullptr;          // 9pt for the count label
HBRUSH   hWindowBrush = nullptr;          // Solid fill matching g_windowColor; used as class bg brush
bool     g_isDarkMode  = false;           // Mirrors the system Apps light/dark preference
COLORREF g_windowColor = RGB(245, 245, 245); // Light mode default; updated by ApplyModernWindowEffects

// ---------------------------------------------------------------------------
// ListView sort state
// ---------------------------------------------------------------------------

// kSortColumnNatural (-1) means no explicit column sort — rows follow the
// order defined by CompareNaturalRowOrder (config index for predefined rows,
// alphabetical otherwise).  Clicking a column header sets g_sortColumn to
// that column's index and g_sortAscending to true; clicking again toggles direction.
int  g_sortColumn    = kSortColumnNatural;
bool g_sortAscending = true;

// ---------------------------------------------------------------------------
// Thread-safe per-process state
// ---------------------------------------------------------------------------

// g_statusLock guards both maps below.  Initialised in WM_CREATE via
// InitializeCriticalSection; intentionally NOT deleted in WM_DESTROY because
// a worker thread that passed the g_shutdownPending check may still be inside
// a critical section at exit time.  The OS reclaims all resources on process exit.
CRITICAL_SECTION                     g_statusLock;

// Last stored action label for each normalized process key (e.g. "msmpeng").
// Entries are removed when the action is kActionNone or empty.
std::map<std::wstring, std::wstring> g_lastActionByName;

// Last-known running state per process, updated each RefreshProcessList tick.
// Used by the action-label state machine to detect transitions (e.g. not-running
// → running = "restarted").
std::map<std::wstring, bool>         g_prevRunningByName;

// ---------------------------------------------------------------------------
// Shutdown coordination
// ---------------------------------------------------------------------------

// Set to true in WM_CLOSE.  Worker threads check this flag before posting
// any UI updates; the flag also prevents new workers from being created.
std::atomic<bool> g_shutdownPending{false};

// Count of active worker threads.  Incremented on the UI thread BEFORE
// CreateThread (so WM_CLOSE can't race against in-flight thread creation),
// decremented by WorkerGuard in each thread proc.  The last decrement
// re-posts WM_CLOSE if g_shutdownPending is true, allowing DestroyWindow
// to proceed once all workers have finished.
std::atomic<int>  g_activeWorkers{0};
