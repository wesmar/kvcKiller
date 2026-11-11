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

#ifndef min
#define min(a,b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef max
#define max(a,b) (((a) > (b)) ? (a) : (b))
#endif

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
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
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
constexpr int kHeaderHeight       = 72;   // Height of the header panels (mathematically fits content)
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
            SetWindowTheme(header, L"DarkMode_ItemsView", nullptr);
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
int MeasureToggleWidth(HWND hwnd, const WString& text, int dpi) {
    const int minimum = MulDiv(130, dpi, 96);
    if (!hwnd || text.empty()) return minimum;

    HDC hdc = GetDC(hwnd);
    if (!hdc) return minimum;

    HFONT font = hUiFont ? hUiFont : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    const auto oldFont = static_cast<HFONT>(SelectObject(hdc, font));

    SIZE size{};
    GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.size()), &size);

    SelectObject(hdc, oldFont);
    ReleaseDC(hwnd, hdc);

    // Auto-checkbox image, padding, and a little side bearing.
    const int chrome = MulDiv(34, dpi, 96);
    return max(size.cx + chrome, minimum);
}

int MeasureWrappedControlHeight(HWND control, int width) {
    if (!control || width <= 0) return 0;

    const int textLength = GetWindowTextLengthW(control);
    if (textLength <= 0) return 0;

    WString text(static_cast<size_t>(textLength) + 1, L'\0');
    GetWindowTextW(control, text.data(), textLength + 1);
    text.resize(static_cast<size_t>(textLength));

    HDC dc = GetDC(control);
    if (!dc) return 0;

    HFONT font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    const HGDIOBJ oldFont = SelectObject(
        dc, font ? font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));

    RECT bounds{0, 0, width, 1};
    DrawTextW(dc, text.c_str(), -1, &bounds,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);

    SelectObject(dc, oldFont);
    ReleaseDC(control, dc);
    return bounds.bottom - bounds.top;
}

} // namespace

int ScaleForDpi(HWND hwnd, int logicalPixels) {
    const UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 96;
    return MulDiv(logicalPixels, static_cast<int>(dpi ? dpi : 96), 96);
}

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
    StrUtils::StrCopy(lf.lfFaceName, sizeof(lf.lfFaceName) / sizeof(wchar_t),
                      IsWindows11OrLater() ? L"Segoe UI Variable" : L"Segoe UI");
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

void InitDarkModeSupport() {
    HMODULE hUx = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!hUx) return;

    // Ordinal 135: SetPreferredAppMode (0=Default, 1=AllowDark, 2=ForceDark, 3=ForceLight)
    using FnSetPreferredAppMode = BOOL(WINAPI*)(DWORD);
    auto fnSetPreferredAppMode = reinterpret_cast<FnSetPreferredAppMode>(
        GetProcAddress(hUx, MAKEINTRESOURCEA(135)));
    if (fnSetPreferredAppMode) {
        fnSetPreferredAppMode(g_isDarkMode ? 2u : 3u);
    }

    // Ordinal 137: RefreshImmersiveColorPolicyState
    using FnRefresh = void(WINAPI*)();
    auto fnRefresh = reinterpret_cast<FnRefresh>(
        GetProcAddress(hUx, MAKEINTRESOURCEA(137)));
    if (fnRefresh) fnRefresh();
}

void AllowWindowDarkMode(HWND hwnd, bool dark) {
    if (!hwnd) return;
    HMODULE hUx = GetModuleHandleW(L"uxtheme.dll");
    if (!hUx) return;

    // Ordinal 133: AllowDarkModeForWindow
    using FnAllow = BOOL(WINAPI*)(HWND, BOOL);
    auto fnAllow = reinterpret_cast<FnAllow>(
        GetProcAddress(hUx, MAKEINTRESOURCEA(133)));
    if (fnAllow) {
        fnAllow(hwnd, dark ? TRUE : FALSE);
    }
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

    AllowWindowDarkMode(hwnd, g_isDarkMode);
    if (hListView) AllowWindowDarkMode(hListView, g_isDarkMode);
    HWND header = hListView ? ListView_GetHeader(hListView) : nullptr;
    if (header) AllowWindowDarkMode(header, g_isDarkMode);
    if (hShowExtraCheck) AllowWindowDarkMode(hShowExtraCheck, g_isDarkMode);
    if (hHideInactiveBuiltInCheck) AllowWindowDarkMode(hHideInactiveBuiltInCheck, g_isDarkMode);
    if (hParalyzeCheck) AllowWindowDarkMode(hParalyzeCheck, g_isDarkMode);
    if (hKillOnceButton) AllowWindowDarkMode(hKillOnceButton, g_isDarkMode);
    if (hKillDefenderButton) AllowWindowDarkMode(hKillDefenderButton, g_isDarkMode);
    if (hRestoreButton) AllowWindowDarkMode(hRestoreButton, g_isDarkMode);

    if (hShowExtraCheck)
        SetWindowTheme(hShowExtraCheck, g_isDarkMode ? L"DarkMode_Explorer" : nullptr, nullptr);
    if (hHideInactiveBuiltInCheck)
        SetWindowTheme(hHideInactiveBuiltInCheck, g_isDarkMode ? L"DarkMode_Explorer" : nullptr, nullptr);
    if (hParalyzeCheck)
        SetWindowTheme(hParalyzeCheck, g_isDarkMode ? L"DarkMode_Explorer" : nullptr, nullptr);
    if (hKillOnceButton)
        SetWindowTheme(hKillOnceButton, g_isDarkMode ? L"DarkMode_Explorer" : nullptr, nullptr);
    if (hKillDefenderButton)
        SetWindowTheme(hKillDefenderButton, g_isDarkMode ? L"DarkMode_Explorer" : nullptr, nullptr);
    if (hRestoreButton)
        SetWindowTheme(hRestoreButton, g_isDarkMode ? L"DarkMode_Explorer" : nullptr, nullptr);

    // Dark/light title bar.
    BOOL dark = g_isDarkMode ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    HIGHCONTRASTW contrast{sizeof(contrast)};
    const bool highContrast =
        SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)
        && (contrast.dwFlags & HCF_HIGHCONTRASTON);

    bool mica = false;
    if (IsWindows11OrLater() && !highContrast) {
        constexpr DWORD kMica = DWMSBT_MAINWINDOW;
        mica = SUCCEEDED(DwmSetWindowAttribute(
            hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &kMica, sizeof(kMica)));
    }
    // GDI writes zero alpha into the redirection surface. Extending the frame
    // into the whole client on such a window makes the entire UI translucent.
    // Keep the native frame opaque until the client is moved to a Direct2D
    // premultiplied-alpha surface.
    const MARGINS frame{};
    DwmExtendFrameIntoClientArea(hwnd, &frame);
    if (!mica) {
        const COLORREF caption = g_windowColor;
        DwmSetWindowAttribute(
            hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
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
    if (!hwnd) return layout;

    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int clientWidth  = rc.right  - rc.left;
    const int clientHeight = rc.bottom - rc.top;
    const UINT dpi = GetDpiForWindow(hwnd);
    layout.dpi = dpi ? dpi : 96;

    const auto scale = [dpi](int value) {
        return MulDiv(value, static_cast<int>(dpi ? dpi : 96), 96);
    };

    const int margin       = scale(kMarginX);
    const int topMargin    = scale(kTopMargin);
    const int headerPadX   = scale(kHeaderPaddingX);
    const int headerPadY   = scale(kHeaderPaddingY);
    const int headerGapX   = scale(kHeaderGapX);
    const int toggleHeight = scale(kToggleHeight);
    const int toggleGapX   = scale(kToggleGapX);
    const int toggleGapY   = scale(8);
    const int sectionGapY  = scale(kGapY);
    const int buttonWidth  = scale(BUTTON_WIDTH);
    const int buttonHeight = scale(BUTTON_HEIGHT);
    const int buttonGap    = scale(kButtonGap);
    const int countHeight  = scale(kCountHeight);
    const int statusHeight = scale(kStatusHeight);

    // Vertical anchor points calculated bottom-up.
    const int buttonRowY = clientHeight - margin - countHeight - statusHeight
        - buttonHeight - scale(18);
    const int listWidth = max(0, clientWidth - (margin * 2));

    // Split the header area ~36/64 between info and warning panels.
    const int headerWidth = max(0, listWidth - headerGapX);
    const int leftPanelWidth = max(scale(260),
        min(scale(380), (headerWidth * 36) / 100));
    const int rightPanelWidth = max(0, headerWidth - leftPanelWidth);

    // Header height dynamically follows the measured text content + padding.
    const int leftTextHeight = MeasureWrappedControlHeight(
        hHeaderInfoText, max(0, leftPanelWidth - headerPadX * 2));
    const int rightTextHeight = MeasureWrappedControlHeight(
        hHeaderWarningText, max(0, rightPanelWidth - headerPadX * 2));
    const int neededTextHeight = max(leftTextHeight, rightTextHeight);
    const int headerHeight = neededTextHeight > 0
        ? max(scale(kHeaderHeight), neededTextHeight + headerPadY * 2 + scale(2))
        : scale(kHeaderHeight);

    layout.headerInfoPanel = MakeRect(margin, topMargin, leftPanelWidth, headerHeight);
    layout.headerWarningPanel = MakeRect(
        margin + leftPanelWidth + headerGapX, topMargin,
        rightPanelWidth, headerHeight);
    layout.headerInfoText     = MakeRect(layout.headerInfoPanel.left  + headerPadX,
                                          layout.headerInfoPanel.top   + headerPadY + scale(2),
                                          max(0, leftPanelWidth - headerPadX * 2),
                                          max(0, headerHeight - headerPadY * 2 - scale(2)));
    layout.headerWarningText = MakeRect(
        layout.headerWarningPanel.left + headerPadX,
        layout.headerWarningPanel.top + headerPadY + scale(2),
        max(0, rightPanelWidth - headerPadX * 2),
        max(0, headerHeight - headerPadY * 2 - scale(2)));

    // Lay out toggles at their measured width. If the row cannot fit them,
    // continue on the next line without truncating any label.
    int x = margin;
    int y = topMargin + headerHeight + scale(10);
    const int rowRight = margin + listWidth;
    auto nextToggle = [&](int desiredWidth) {
        const int width = min(desiredWidth, listWidth);
        if (x != margin && x + width > rowRight) {
            x = margin;
            y += toggleHeight + toggleGapY;
        }
        const RECT result = MakeRect(x, y, width, toggleHeight);
        x += width + toggleGapX;
        return result;
    };

    const int showExtraWidth = MeasureToggleWidth(hwnd, LoadStr(IDS_CHK_SHOW_EXTRA), static_cast<int>(dpi));
    const int paralyzeWidth = MeasureToggleWidth(hwnd, LoadStr(IDS_CHK_PARALYZE), static_cast<int>(dpi));
    const int hideInactiveWidth = MeasureToggleWidth(hwnd, LoadStr(IDS_CHK_HIDE_INACTIVE), static_cast<int>(dpi));

    layout.showExtraToggle = nextToggle(showExtraWidth);
    const RECT paralyzeBounds = nextToggle(paralyzeWidth);
    const int paralyzeBoxWidth = scale(15);
    layout.paralyzeCheck = MakeRect(
        paralyzeBounds.left, paralyzeBounds.top,
        paralyzeBoxWidth, paralyzeBounds.bottom - paralyzeBounds.top);
    layout.paralyzeLabel = MakeRect(
        layout.paralyzeCheck.right + scale(4), paralyzeBounds.top + 1,
        max(0, paralyzeBounds.right - layout.paralyzeCheck.right - scale(4)),
        paralyzeBounds.bottom - paralyzeBounds.top - 1);
    layout.hideInactiveToggle = nextToggle(hideInactiveWidth);

    const int listY = y + toggleHeight + sectionGapY;
    const int listHeight = max(scale(150), buttonRowY - sectionGapY - listY);
    layout.list = MakeRect(margin, listY, listWidth, listHeight);

    // Three equal buttons remain centered at the original window width.
    const int totalButtonsWidth = buttonWidth * 3 + buttonGap * 2;
    const int actualButtonWidth = totalButtonsWidth <= listWidth
        ? buttonWidth
        : max(scale(120), (listWidth - buttonGap * 2) / 3);
    const int actualButtonsWidth = actualButtonWidth * 3 + buttonGap * 2;
    const int buttonStartX = margin + max(0, (listWidth - actualButtonsWidth) / 2);

    layout.killButton = MakeRect(
        buttonStartX, buttonRowY, actualButtonWidth, buttonHeight);
    layout.killDefenderButton = MakeRect(
        layout.killButton.right + buttonGap, buttonRowY,
        actualButtonWidth, buttonHeight);
    layout.restoreButton = MakeRect(
        layout.killDefenderButton.right + buttonGap, buttonRowY,
        actualButtonWidth, buttonHeight);
    layout.status = MakeRect(
        margin, buttonRowY + buttonHeight + scale(10),
        listWidth, statusHeight);
    layout.count = MakeRect(
        margin, clientHeight - margin - countHeight,
        listWidth, countHeight);

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
        !hKillOnceButton || !hKillDefenderButton     || !hRestoreButton ||
        !hStatusText     || !hProcessCount)
        return;

    const MainLayoutRects layout = CalculateMainLayout(hwnd);

    auto place = [](HWND control, const RECT& rect) {
        SetWindowPos(control, nullptr, rect.left, rect.top,
                     rect.right - rect.left, rect.bottom - rect.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    };

    place(hHeaderInfoText, layout.headerInfoText);
    place(hHeaderWarningText, layout.headerWarningText);
    place(hShowExtraCheck, layout.showExtraToggle);
    place(hHideInactiveBuiltInCheck, layout.hideInactiveToggle);
    place(hListView, layout.list);
    place(hParalyzeCheck, layout.paralyzeCheck);
    place(hParalyzeLabel, layout.paralyzeLabel);
    place(hKillOnceButton, layout.killButton);
    place(hKillDefenderButton, layout.killDefenderButton);
    place(hRestoreButton, layout.restoreButton);
    place(hStatusText, layout.status);
    place(hProcessCount, layout.count);

    // Column 0 (Name) takes up all remaining space after the fixed-width columns.
    // Subtract 8 extra pixels to avoid a horizontal scrollbar from rounding.
    const int nameWidth = max(ScaleForDpi(hwnd, kColNameMinCx),
        (layout.list.right - layout.list.left)
        - ScaleForDpi(hwnd, kColSourceCx) - ScaleForDpi(hwnd, kColPidCx)
        - ScaleForDpi(hwnd, kColStatusCx) - ScaleForDpi(hwnd, kColLastActionCx)
        - ScaleForDpi(hwnd, 8));

    ListView_SetColumnWidth(hListView, 0, nameWidth);
    ListView_SetColumnWidth(hListView, 1, ScaleForDpi(hwnd, kColSourceCx));
    ListView_SetColumnWidth(hListView, 2, ScaleForDpi(hwnd, kColPidCx));
    ListView_SetColumnWidth(hListView, 3, ScaleForDpi(hwnd, kColStatusCx));
    ListView_SetColumnWidth(hListView, 4, ScaleForDpi(hwnd, kColLastActionCx));
}

// ---------------------------------------------------------------------------
// Informational helpers
// ---------------------------------------------------------------------------

// Reads the human-readable Windows version ("24H2") and build number from the
// registry and returns them as a single string ("24H2 (Build 26100)").
// Returns an empty string if the registry key is missing (shouldn't happen).
WString GetWindowsVersion() {
    HKEY hKey;
    DWORD dwType, dwSize;
    wchar_t version[256] = {};
    wchar_t build[256]   = {};
    WString result;

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
        result += L" (Build " + WString(build) + L")";

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
void UpdateStatusText(const WString& text) {
    WString* pText = new WString(text);
    if (!PostMessage(hInstMain, WMU_STATUS_TEXT, reinterpret_cast<WPARAM>(pText), 0))
        delete pText; // PostMessage failed (e.g., window destroyed) — don't leak.
}

// Updates the process-count label at the bottom of the window.
// Must be called from the UI thread (directly updates the static control).
void UpdateProcessCount(int targetCount, int runningCount, int extraCount) {
    wchar_t buffer[128];
    wsprintfW(buffer,
              L"Targets: %d | Running: %d | Extra live: %d | Visible: %d",
              targetCount, runningCount, extraCount,
              ListView_GetItemCount(hListView));
    SetWindowTextW(hProcessCount, buffer);
}
