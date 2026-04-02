// UIHelpers.cpp
// © WESMAR Marek Wesołowski
//
// Implements all UI infrastructure that isn't business logic:
//   - Font creation (DPI-aware, Segoe UI / Segoe UI Variable).
//   - Dark/light mode detection and DWM attribute application.
//   - ListView visual theme and color sync.
//   - Main-window layout calculation and MoveWindow application.
//   - Status-bar and process-count label updates.

#include "UIHelpers.h"
#include "GlobalData.h"
#include "Utils.h"
#include "Resource.h"

#include <dwmapi.h>
#include <commctrl.h>
#include <uxtheme.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

// DWM attribute IDs that were added in Windows 10 20H1 / Windows 11 and may
// be absent from older SDK headers.  Define them here so the TU compiles on
// any SDK version and falls back gracefully at runtime on older OS builds.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMSBT_MAINWINDOW
#define DWMSBT_MAINWINDOW 2  // Mica material backdrop
#endif

namespace {

// --- Color constants for light/dark themes ---
constexpr COLORREF kLightWindowColor = RGB(245, 245, 245);
constexpr COLORREF kDarkWindowColor  = RGB(40, 40, 40);
constexpr COLORREF kDarkListColor    = RGB(32, 32, 32);
constexpr COLORREF kLightListColor   = RGB(255, 255, 255);

// --- Layout constants (all in logical pixels at 100% DPI) ---
constexpr int kMarginX            = 24;   // Left/right outer margin
constexpr int kTopMargin          = 16;   // Top margin above header panels
constexpr int kHeaderHeight       = 112;  // Height of the header panels
constexpr int kHeaderGapX         = 18;   // Horizontal gap between the two header panels
constexpr int kHeaderPaddingX     = 14;   // Horizontal padding inside each header panel
constexpr int kHeaderPaddingY     = 12;   // Vertical padding inside each header panel
constexpr int kToggleHeight       = 24;   // Height of the checkbox row
constexpr int kToggleGapX         = 24;   // Horizontal gap between checkboxes
constexpr int kGapY               = 14;   // Vertical gap between the toggle row and the ListView
constexpr int kButtonGap          = 24;   // Horizontal gap between Kill and Restore buttons
constexpr int kCountHeight        = 22;   // Height of the process-count label
constexpr int kStatusHeight       = 28;   // Height of the status text label

// Recreates hWindowBrush with the new color and updates the window class
// background so WM_ERASEBKGND uses the correct brush without manual handling.
void RecreateWindowBrush(HWND hwnd, COLORREF color) {
    if (hWindowBrush) {
        DeleteObject(hWindowBrush);
        hWindowBrush = nullptr;
    }

    hWindowBrush  = CreateSolidBrush(color);
    g_windowColor = color;

    if (hwnd && hWindowBrush) {
        // Update the WNDCLASS background so newly exposed areas erase correctly.
        SetClassLongPtrW(hwnd, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(hWindowBrush));
    }
}

// Applies the appropriate visual theme and color scheme to the ListView and
// its header control.  Must be called whenever g_isDarkMode changes.
void ApplyListViewTheme() {
    if (!hListView) {
        return;
    }

    HWND header = ListView_GetHeader(hListView);
    if (g_isDarkMode) {
        SetWindowTheme(hListView, L"DarkMode_Explorer", nullptr);
        ListView_SetBkColor(hListView, kDarkListColor);
        ListView_SetTextBkColor(hListView, kDarkListColor);
        ListView_SetTextColor(hListView, RGB(235, 235, 235));
        if (header) {
            SetWindowTheme(header, L"DarkMode_Header", nullptr);
        }
    } else {
        SetWindowTheme(hListView, L"Explorer", nullptr);
        ListView_SetBkColor(hListView, kLightListColor);
        ListView_SetTextBkColor(hListView, kLightListColor);
        ListView_SetTextColor(hListView, RGB(32, 32, 32));
        if (header) {
            SetWindowTheme(header, L"Header", nullptr);
        }
    }
}

// Constructs a RECT from origin + size rather than origin + end-point,
// which is easier to read in layout code.
RECT MakeRect(int left, int top, int width, int height) {
    RECT rc{};
    rc.left   = left;
    rc.top    = top;
    rc.right  = left + width;
    rc.bottom = top + height;
    return rc;
}

// Measures the pixel width needed to display a checkbox + label, clamped to
// [minWidth, maxWidth].  The +34 adds space for the checkbox square and
// a few pixels of breathing room on each side.
int MeasureToggleWidth(HWND hwnd, const std::wstring& text, int minWidth, int maxWidth) {
    if (!hwnd || text.empty()) {
        return minWidth;
    }

    HDC hdc = GetDC(hwnd);
    if (!hdc) {
        return minWidth;
    }

    HFONT font = hUiFont ? hUiFont : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    const auto oldFont = static_cast<HFONT>(SelectObject(hdc, font));

    SIZE size{};
    GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.size()), &size);

    SelectObject(hdc, oldFont);
    ReleaseDC(hwnd, hdc);

    // Checkbox square + breathing room around the label.
    return min(max(size.cx + 34, minWidth), maxWidth);
}

} // namespace

// ---------------------------------------------------------------------------
// Font helpers
// ---------------------------------------------------------------------------

// Creates a ClearType font at the given point size and weight, scaled to the
// window's current DPI.  Uses "Segoe UI Variable" on Windows 11+ (better
// optical sizing at small sizes) and falls back to "Segoe UI" on older builds.
HFONT CreateUiFont(HWND hwnd, int pointSize, int weight) {
    LOGFONTW lf = {};
    const UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 96;
    // Negative height = character height in logical units (standard practice
    // for point-size fonts — positive height includes internal leading).
    lf.lfHeight  = -MulDiv(pointSize, static_cast<int>(dpi), 72);
    lf.lfWeight  = weight;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, IsWindows11OrLater() ? L"Segoe UI Variable" : L"Segoe UI");
    return CreateFontIndirectW(&lf);
}

// ---------------------------------------------------------------------------
// Window appearance
// ---------------------------------------------------------------------------

// Reads the "AppsUseLightTheme" registry value from the Personalize key.
// Returns true if dark mode is active (value == 0) or if the key is missing.
bool AppUseDarkMode() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    DWORD value = 1; // Default: light mode
    DWORD size  = sizeof(value);
    RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr,
                     reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(hKey);
    return value == 0; // 0 = dark mode active
}

// Applies all DWM-level visual effects and re-syncs child controls.
// Call this on WM_CREATE, WM_THEMECHANGED, and WM_SETTINGCHANGE.
//
// Effects applied:
//   - Immersive dark mode title bar.
//   - Mica backdrop on Windows 11 (DWMSBT_MAINWINDOW).
//   - Rounded corners (DWMWCP_ROUND).
//   - ListView and header visual themes.
//   - Invalidates all static controls so WM_CTLCOLORSTATIC fires with the
//     new palette.
void ApplyModernWindowEffects(HWND hwnd) {
    if (!hwnd) return;

    g_isDarkMode = AppUseDarkMode();
    RecreateWindowBrush(hwnd, g_isDarkMode ? kDarkWindowColor : kLightWindowColor);

    // Dark/light title bar.
    BOOL dark = g_isDarkMode ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    if (IsWindows11OrLater()) {
        // Mica material — the window background blends with the desktop wallpaper.
        constexpr DWORD kMica = DWMSBT_MAINWINDOW;
        DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &kMica, sizeof(kMica));
    }

    // Rounded corners — cosmetic only, no impact on hit testing.
    constexpr DWORD kRound = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &kRound, sizeof(kRound));

    ApplyListViewTheme();

    // Force-repaint every static control so they pick up the new color scheme.
    if (hStatusText)              InvalidateRect(hStatusText,              nullptr, TRUE);
    if (hProcessCount)            InvalidateRect(hProcessCount,            nullptr, TRUE);
    if (hHeaderInfoText)          InvalidateRect(hHeaderInfoText,          nullptr, TRUE);
    if (hHeaderWarningText)       InvalidateRect(hHeaderWarningText,       nullptr, TRUE);
    if (hShowExtraCheck)          InvalidateRect(hShowExtraCheck,          nullptr, TRUE);
    if (hHideInactiveBuiltInCheck)InvalidateRect(hHideInactiveBuiltInCheck,nullptr, TRUE);
    if (hParalyzeCheck)           InvalidateRect(hParalyzeCheck,           nullptr, TRUE);
    if (hParalyzeLabel)           InvalidateRect(hParalyzeLabel,           nullptr, TRUE);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

// Computes all child-control rectangles from the current client area.
// Returns a MainLayoutRects struct so callers can either apply the rects
// immediately (LayoutMainWindow) or inspect specific rects (e.g., for
// WM_PAINT header panel drawing).
//
// Toggle-row widths are measured against the actual UI font so the layout
// adapts to localized strings.  If the combined width exceeds the list width,
// each toggle is shrunk and/or the gaps are reduced proportionally.
MainLayoutRects CalculateMainLayout(HWND hwnd) {
    MainLayoutRects layout{};
    if (!hwnd) {
        return layout;
    }

    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int clientWidth  = rc.right  - rc.left;
    const int clientHeight = rc.bottom - rc.top;

    // Vertical anchor points calculated bottom-up.
    const int buttonRowY  = clientHeight - kMarginX - kCountHeight - kStatusHeight - BUTTON_HEIGHT - 18;
    const int toggleY     = kTopMargin + kHeaderHeight + 10;
    const int listY       = toggleY + kToggleHeight + kGapY;
    const int listHeight  = max(180, buttonRowY - kGapY - listY);
    const int listWidth   = clientWidth - (kMarginX * 2);

    // Split the header area ~34/66 between info and warning panels.
    const int headerWidth     = max(0, listWidth - kHeaderGapX);
    const int leftPanelWidth  = max(250, min(360, (headerWidth * 34) / 100));
    const int rightPanelWidth = max(0, headerWidth - leftPanelWidth);

    // Center the Kill/Restore pair horizontally.
    const int totalButtonsWidth = (BUTTON_WIDTH * 2) + kButtonGap;
    const int startX            = (clientWidth - totalButtonsWidth) / 2;

    layout.headerInfoPanel    = MakeRect(kMarginX, kTopMargin, leftPanelWidth, kHeaderHeight);
    layout.headerWarningPanel = MakeRect(kMarginX + leftPanelWidth + kHeaderGapX,
                                          kTopMargin, rightPanelWidth, kHeaderHeight);
    layout.headerInfoText     = MakeRect(layout.headerInfoPanel.left  + kHeaderPaddingX,
                                          layout.headerInfoPanel.top   + kHeaderPaddingY,
                                          max(0, leftPanelWidth  - (kHeaderPaddingX * 2)),
                                          max(0, kHeaderHeight   - (kHeaderPaddingY * 2)));
    layout.headerWarningText  = MakeRect(layout.headerWarningPanel.left + kHeaderPaddingX,
                                          layout.headerWarningPanel.top  + kHeaderPaddingY,
                                          max(0, rightPanelWidth - (kHeaderPaddingX * 2)),
                                          max(0, kHeaderHeight   - (kHeaderPaddingY * 2)));

    // Measure each toggle against the real font/locale text.
    int showExtraWidth    = MeasureToggleWidth(hwnd, LoadStr(IDS_CHK_SHOW_EXTRA),   250, max(250, listWidth / 2));
    int paralyzeWidth     = MeasureToggleWidth(hwnd, LoadStr(IDS_CHK_PARALYZE),     180, 260);
    int hideInactiveWidth = MeasureToggleWidth(hwnd, LoadStr(IDS_CHK_HIDE_INACTIVE),220, 300);

    int gapX             = kToggleGapX;
    int totalToggleWidth = showExtraWidth + paralyzeWidth + hideInactiveWidth + (gapX * 2);

    // First shrink pass: reduce the widest toggle if combined width overflows.
    if (totalToggleWidth > listWidth) {
        const int shrink = totalToggleWidth - listWidth;
        showExtraWidth   = max(220, showExtraWidth - shrink);
        totalToggleWidth = showExtraWidth + paralyzeWidth + hideInactiveWidth + (gapX * 2);
    }
    // Second shrink pass: reduce the gaps if toggling alone wasn't enough.
    if (totalToggleWidth > listWidth) {
        gapX             = max(12, (listWidth - (showExtraWidth + paralyzeWidth + hideInactiveWidth)) / 2);
        totalToggleWidth = showExtraWidth + paralyzeWidth + hideInactiveWidth + (gapX * 2);
    }

    // The paralyze checkbox is split: a narrow box (15 px) + the label text.
    // This prevents the OS focus rectangle from bleeding into adjacent controls.
    int paralyzeBoxWidth  = 15;
    int paralyzeTextWidth = paralyzeWidth - paralyzeBoxWidth;

    const int toggleStartX = kMarginX + max(0, (listWidth - totalToggleWidth) / 2);

    layout.showExtraToggle    = MakeRect(toggleStartX, toggleY, showExtraWidth, kToggleHeight);
    layout.paralyzeCheck      = MakeRect(layout.showExtraToggle.right + gapX,
                                          toggleY, paralyzeBoxWidth, kToggleHeight);
    layout.paralyzeLabel      = MakeRect(layout.paralyzeCheck.right + 4,
                                          toggleY + 1, paralyzeTextWidth, kToggleHeight - 1);
    layout.hideInactiveToggle = MakeRect(layout.showExtraToggle.right + gapX + paralyzeWidth + gapX,
                                          toggleY, hideInactiveWidth, kToggleHeight);
    layout.list               = MakeRect(kMarginX, listY, listWidth, listHeight);
    layout.killButton         = MakeRect(startX, buttonRowY, BUTTON_WIDTH, BUTTON_HEIGHT);
    layout.restoreButton      = MakeRect(startX + BUTTON_WIDTH + kButtonGap, buttonRowY,
                                          BUTTON_WIDTH, BUTTON_HEIGHT);
    layout.status             = MakeRect(kMarginX, buttonRowY + BUTTON_HEIGHT + 10,
                                          listWidth, kStatusHeight);
    layout.count              = MakeRect(kMarginX, clientHeight - kMarginX - kCountHeight,
                                          listWidth, kCountHeight);

    return layout;
}

// Moves every child control to the positions returned by CalculateMainLayout
// and resizes the ListView columns to fill the available width.
// Guard-checked at the top — safe to call before WM_CREATE finishes
// (early WM_SIZE messages on Windows 11 can arrive before all handles exist).
void LayoutMainWindow(HWND hwnd) {
    if (!hwnd           || !hHeaderInfoText         || !hHeaderWarningText      ||
        !hListView       || !hShowExtraCheck         || !hHideInactiveBuiltInCheck ||
        !hParalyzeCheck  || !hParalyzeLabel          ||
        !hKillOnceButton || !hRestoreButton          ||
        !hStatusText     || !hProcessCount)
        return;

    const MainLayoutRects layout = CalculateMainLayout(hwnd);

    // Move every child using the precomputed rects.  The MoveWindow calls are
    // written out individually (rather than in a loop) for readability and so
    // each control maps clearly to its layout rect field.
    MoveWindow(hHeaderInfoText,
               layout.headerInfoText.left, layout.headerInfoText.top,
               layout.headerInfoText.right  - layout.headerInfoText.left,
               layout.headerInfoText.bottom - layout.headerInfoText.top, TRUE);
    MoveWindow(hHeaderWarningText,
               layout.headerWarningText.left, layout.headerWarningText.top,
               layout.headerWarningText.right  - layout.headerWarningText.left,
               layout.headerWarningText.bottom - layout.headerWarningText.top, TRUE);
    MoveWindow(hShowExtraCheck,
               layout.showExtraToggle.left, layout.showExtraToggle.top,
               layout.showExtraToggle.right  - layout.showExtraToggle.left,
               layout.showExtraToggle.bottom - layout.showExtraToggle.top, TRUE);
    MoveWindow(hHideInactiveBuiltInCheck,
               layout.hideInactiveToggle.left, layout.hideInactiveToggle.top,
               layout.hideInactiveToggle.right  - layout.hideInactiveToggle.left,
               layout.hideInactiveToggle.bottom - layout.hideInactiveToggle.top, TRUE);
    MoveWindow(hListView,
               layout.list.left, layout.list.top,
               layout.list.right  - layout.list.left,
               layout.list.bottom - layout.list.top, TRUE);
    MoveWindow(hParalyzeCheck,
               layout.paralyzeCheck.left, layout.paralyzeCheck.top,
               layout.paralyzeCheck.right  - layout.paralyzeCheck.left,
               layout.paralyzeCheck.bottom - layout.paralyzeCheck.top, TRUE);
    MoveWindow(hParalyzeLabel,
               layout.paralyzeLabel.left, layout.paralyzeLabel.top,
               layout.paralyzeLabel.right  - layout.paralyzeLabel.left,
               layout.paralyzeLabel.bottom - layout.paralyzeLabel.top, TRUE);
    MoveWindow(hKillOnceButton,
               layout.killButton.left, layout.killButton.top,
               layout.killButton.right  - layout.killButton.left,
               layout.killButton.bottom - layout.killButton.top, TRUE);
    MoveWindow(hRestoreButton,
               layout.restoreButton.left, layout.restoreButton.top,
               layout.restoreButton.right  - layout.restoreButton.left,
               layout.restoreButton.bottom - layout.restoreButton.top, TRUE);
    MoveWindow(hStatusText,
               layout.status.left, layout.status.top,
               layout.status.right  - layout.status.left,
               layout.status.bottom - layout.status.top, TRUE);
    MoveWindow(hProcessCount,
               layout.count.left, layout.count.top,
               layout.count.right  - layout.count.left,
               layout.count.bottom - layout.count.top, TRUE);

    // Column 0 (Name) takes up all remaining space after the fixed-width columns.
    // Subtract 8 extra pixels to avoid a horizontal scrollbar from rounding.
    const int nameWidth = max(kColNameMinCx,
        (layout.list.right - layout.list.left)
        - kColSourceCx - kColPidCx - kColStatusCx - kColLastActionCx - 8);

    ListView_SetColumnWidth(hListView, 0, nameWidth);
    ListView_SetColumnWidth(hListView, 1, kColSourceCx);
    ListView_SetColumnWidth(hListView, 2, kColPidCx);
    ListView_SetColumnWidth(hListView, 3, kColStatusCx);
    ListView_SetColumnWidth(hListView, 4, kColLastActionCx);
}

// ---------------------------------------------------------------------------
// Informational helpers
// ---------------------------------------------------------------------------

// Reads the human-readable Windows version ("24H2") and build number from the
// registry and returns them as a single string ("24H2 (Build 26100)").
// Returns an empty string if the registry key is missing (shouldn't happen).
std::wstring GetWindowsVersion() {
    HKEY hKey;
    DWORD dwType, dwSize;
    wchar_t version[256] = {};
    wchar_t build[256]   = {};
    std::wstring result;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                      0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return result;

    dwSize = sizeof(version);
    if (RegQueryValueExW(hKey, L"DisplayVersion", nullptr, &dwType,
                         reinterpret_cast<LPBYTE>(version), &dwSize) == ERROR_SUCCESS)
        result = version;

    dwSize = sizeof(build);
    if (RegQueryValueExW(hKey, L"CurrentBuildNumber", nullptr, &dwType,
                         reinterpret_cast<LPBYTE>(build), &dwSize) == ERROR_SUCCESS)
        result += L" (Build " + std::wstring(build) + L")";

    RegCloseKey(hKey);
    return result;
}

// ---------------------------------------------------------------------------
// Status-bar / process-count updates
// ---------------------------------------------------------------------------

// Posts the status text to the main window from any thread.
// The string is heap-allocated here and ownership is transferred to
// WMU_STATUS_TEXT in WndProc, which deletes it after SetWindowTextW.
// PostMessage is used (not SendMessage) to avoid blocking worker threads.
void UpdateStatusText(const std::wstring& text) {
    std::wstring* pText = new std::wstring(text);
    if (!PostMessage(hInstMain, WMU_STATUS_TEXT, reinterpret_cast<WPARAM>(pText), 0))
        delete pText; // PostMessage failed (e.g., window destroyed) — don't leak.
}

// Updates the process-count label at the bottom of the window.
// Must be called from the UI thread (directly updates the static control).
void UpdateProcessCount(int targetCount, int runningCount, int extraCount) {
    wchar_t buffer[128];
    swprintf_s(buffer,
               L"Targets: %d | Running: %d | Extra live: %d | Visible: %d",
               targetCount, runningCount, extraCount,
               ListView_GetItemCount(hListView));
    SetWindowTextW(hProcessCount, buffer);
}
