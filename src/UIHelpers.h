// UIHelpers.h
// UI helpers: font management, modern window effects, main-window layout,
// and thread-safe status-bar update helpers.
// © WESMAR Marek Wesołowski

#pragma once

#include <windows.h>
#include <string>

struct MainLayoutRects {
    RECT headerInfoPanel{};
    RECT headerInfoText{};
    RECT headerWarningPanel{};
    RECT headerWarningText{};
    RECT showExtraToggle{};
    RECT hideInactiveToggle{};
    RECT list{};
    RECT paralyzeCheck{};
    RECT paralyzeLabel{};
    RECT killButton{};
    RECT restoreButton{};
    RECT status{};
    RECT count{};
};

// ---------------------------------------------------------------------------
// Font / GDI object helpers
// ---------------------------------------------------------------------------

/// Creates a ClearType UI font at the given point size, DPI-aware.
/// weight defaults to FW_NORMAL; pass FW_SEMIBOLD / FW_BOLD as needed.
HFONT CreateUiFont(HWND hwnd, int pointSize, int weight = FW_NORMAL);

/// Deletes any GDI object (HFONT, HBRUSH, HPEN …) and nulls the handle.
/// Defined inline here so the template instantiates correctly in every TU.
template <typename T>
inline void DeleteUiObject(T& obj) {
    if (obj) {
        DeleteObject(obj);
        obj = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Window appearance
// ---------------------------------------------------------------------------

/// Returns true when the system app theme preference is dark.
bool AppUseDarkMode();

/// Applies Mica backdrop (Win 11) and rounded corners to \p hwnd.
void ApplyModernWindowEffects(HWND hwnd);

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

/// Computes the layout rectangles for the current client size.
MainLayoutRects CalculateMainLayout(HWND hwnd);

/// Repositions all child controls to fill the current client area.
/// Safe to call from WM_SIZE and WM_CREATE.
void LayoutMainWindow(HWND hwnd);

// ---------------------------------------------------------------------------
// Informational helpers
// ---------------------------------------------------------------------------

/// Reads DisplayVersion + CurrentBuildNumber from the registry.
std::wstring GetWindowsVersion();

// ---------------------------------------------------------------------------
// Status-bar / process-count updates
// ---------------------------------------------------------------------------

/// Thread-safe: heap-allocates the string and posts WMU_STATUS_TEXT
/// to the main window; the message handler deletes the allocation.
void UpdateStatusText(const std::wstring& text);

/// Updates the process-count footer label (must be called from the UI thread).
void UpdateProcessCount(int targetCount, int runningCount, int extraCount);
