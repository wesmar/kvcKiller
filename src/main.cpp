// main.cpp
// KvcKiller — main application entry point and window procedure.
// All application logic lives in the dedicated modules below.
// Author: WESMAR Marek Wesołowski

#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <cmath>

#include "GlobalData.h"
#include "Utils.h"
#include "UIHelpers.h"
#include "ListViewManager.h"
#include "ProcessOperations.h"
#include "CLIHandler.h"
#include "Resource.h"

#pragma comment(lib, "comctl32.lib")

namespace {

COLORREF GetParalyzePulseColor() {
    const double t     = static_cast<double>(GetTickCount64() % 4000) / 4000.0;
    const double pulse = (std::sin(t * 6.283185307179586) + 1.0) / 2.0; // 0.0–1.0
    const double f     = 0.3 + 0.7 * pulse;

    const COLORREF redColor = RGB(255, 40, 40);
    const COLORREF bgColor  = g_windowColor;

    const int r = static_cast<int>(GetRValue(redColor) * f + GetRValue(bgColor) * (1.0 - f));
    const int g = static_cast<int>(GetGValue(redColor) * f + GetGValue(bgColor) * (1.0 - f));
    const int b = static_cast<int>(GetBValue(redColor) * f + GetBValue(bgColor) * (1.0 - f));
    return RGB(r, g, b);
}

void PaintParalyzeLabel(HWND hwnd, HDC targetHdc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int width  = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    HDC memDc = CreateCompatibleDC(targetHdc);
    HBITMAP memBmp = CreateCompatibleBitmap(targetHdc, width, height);
    const auto oldBmp = static_cast<HBITMAP>(SelectObject(memDc, memBmp));

    FillRect(memDc, &rc, hWindowBrush);

    const auto font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    const auto oldFont = static_cast<HFONT>(SelectObject(
        memDc, font ? font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT))));

    SetBkMode(memDc, TRANSPARENT);
    SetTextColor(memDc, GetParalyzePulseColor());

    const int textLen = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<size_t>(textLen) + 1, L'\0');
    if (textLen > 0) {
        GetWindowTextW(hwnd, text.data(), textLen + 1);
    }

    RECT textRc = rc;
	textRc.top -= 2;
    DrawTextW(memDc, text.c_str(), -1, &textRc,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    BitBlt(targetHdc, 0, 0, width, height, memDc, 0, 0, SRCCOPY);

    SelectObject(memDc, oldFont);
    SelectObject(memDc, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDc);
}

LRESULT CALLBACK ParalyzeLabelSubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintParalyzeLabel(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_PRINTCLIENT:
        PaintParalyzeLabel(hwnd, reinterpret_cast<HDC>(wParam));
        return 0;

    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, ParalyzeLabelSubclassProc, 1);
        break;
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// Draws a single header info/warning panel — rounded rectangle with a two-layer
// border (outer dark, inner highlight) and a thin accent line near the top.
// Called twice per WM_PAINT: once for the info panel, once for the warning panel.
void DrawHeaderPanel(HDC hdc, RECT rc) {
    const COLORREF fillColor       = g_isDarkMode ? RGB(46, 49, 54)  : RGB(252, 253, 255);
    const COLORREF borderColor     = g_isDarkMode ? RGB(82, 88, 100) : RGB(205, 212, 222);
    const COLORREF innerBorderColor= g_isDarkMode ? RGB(60, 66, 78)  : RGB(255, 255, 255);
    const COLORREF accentColor     = g_isDarkMode ? RGB(110, 146, 204): RGB(166, 194, 233);

    HPEN outerPen  = CreatePen(PS_SOLID, 1, borderColor);
    HPEN innerPen  = CreatePen(PS_SOLID, 1, innerBorderColor);
    HPEN accentPen = CreatePen(PS_SOLID, 1, accentColor);
    HBRUSH fillBrush = CreateSolidBrush(fillColor);

    const auto oldPen   = static_cast<HPEN>(SelectObject(hdc, outerPen));
    const auto oldBrush = static_cast<HBRUSH>(SelectObject(hdc, fillBrush));

    // Outer rounded rect — 10px corner radius.
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 10, 10);

    // Inner highlight border sits 1px inside the outer one.
    RECT inner = rc;
    InflateRect(&inner, -1, -1);
    SelectObject(hdc, innerPen);
    RoundRect(hdc, inner.left, inner.top, inner.right, inner.bottom, 9, 9);

    // Thin horizontal accent line near the top, inset by 16px on each side.
    SelectObject(hdc, accentPen);
    MoveToEx(hdc, rc.left + 16, rc.top + 9, nullptr);
    LineTo(hdc,   rc.right - 16, rc.top + 9);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);

    DeleteObject(fillBrush);
    DeleteObject(accentPen);
    DeleteObject(innerPen);
    DeleteObject(outerPen);
}

// Clears the combined header area with the window background color, then
// draws both header panels.  Called from WM_PAINT.
void PaintHeaderPanels(HWND hwnd, HDC hdc) {
    const MainLayoutRects layout = CalculateMainLayout(hwnd);
    // Build a bounding rect that covers both panels so the fill is clean.
    RECT headerBounds = layout.headerInfoPanel;
    headerBounds.right  = layout.headerWarningPanel.right;
    headerBounds.bottom = max(layout.headerInfoPanel.bottom, layout.headerWarningPanel.bottom);
    FillRect(hdc, &headerBounds, hWindowBrush);
    DrawHeaderPanel(hdc, layout.headerInfoPanel);
    DrawHeaderPanel(hdc, layout.headerWarningPanel);
}

} // namespace

// =============================================================================
// Window Procedure
// =============================================================================

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    // -------------------------------------------------------------------------
    case WM_CREATE: {
        InitializeCriticalSection(&g_statusLock);
        ApplyModernWindowEffects(hwnd);

        // Create all UI fonts.  Points are logical; CreateUiFont scales by DPI.
        hUiFont     = CreateUiFont(hwnd, 9);
        hButtonFont = CreateUiFont(hwnd, 10, FW_SEMIBOLD);
        hStatusFont = CreateUiFont(hwnd, 12, FW_SEMIBOLD);
        hCountFont  = CreateUiFont(hwnd, 9);

        // --- Header panels ---
        // Left panel: author + version info.
        hHeaderInfoText = CreateWindowW(
            L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            0, 0, 100, 40, hwnd, nullptr, hInst, nullptr);
        SendMessage(hHeaderInfoText, WM_SETFONT, reinterpret_cast<WPARAM>(hUiFont), TRUE);
        SetWindowTextW(hHeaderInfoText,
            (LoadStr(IDS_AUTHOR_LINE) + L"\n" +
             LoadStr(IDS_APP_SUBTITLE) + L" - Windows " + GetWindowsVersion()).c_str());

        // Right panel: disclaimer / warning text.
        hHeaderWarningText = CreateWindowW(
            L"STATIC", LoadStr(IDS_APP_WARNING).c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            0, 0, 100, 40, hwnd, nullptr, hInst, nullptr);
        SendMessage(hHeaderWarningText, WM_SETFONT, reinterpret_cast<WPARAM>(hUiFont), TRUE);

        // --- Process ListView ---
        hListView = CreateWindowW(
            WC_LISTVIEW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
            0, 0, 100, 100, hwnd, nullptr, hInst, nullptr);
        SendMessage(hListView, WM_SETFONT, reinterpret_cast<WPARAM>(hUiFont), TRUE);
        InitListView();  // columns + extended styles + theme

        // --- Filter checkboxes ---
        // "Show extra processes" — appends all running processes not in config.
        hShowExtraCheck = CreateWindowW(
            L"BUTTON", LoadStr(IDS_CHK_SHOW_EXTRA).c_str(),
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            0, 0, 100, 24, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SHOW_EXTRA)), hInst, nullptr);
        SendMessage(hShowExtraCheck, WM_SETFONT, reinterpret_cast<WPARAM>(hUiFont), TRUE);

        // "Hide inactive built-in" — collapses rows with no running state and
        // no pending action, keeping the list tidy for long target lists.
        hHideInactiveBuiltInCheck = CreateWindowW(
            L"BUTTON", LoadStr(IDS_CHK_HIDE_INACTIVE).c_str(),
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            0, 0, 100, 24, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_HIDE_INACTIVE_BUILTIN)), hInst, nullptr);
        SendMessage(hHideInactiveBuiltInCheck, WM_SETFONT, reinterpret_cast<WPARAM>(hUiFont), TRUE);

        // --- Paralyze checkbox (split into bare checkbox + clickable label) ---
        // The checkbox square is kept intentionally narrow (paralyzeBoxWidth px)
        // so the OS focus rectangle doesn't overhang into the adjacent controls.
        // A separate SS_NOTIFY STATIC acts as the visible label; STN_CLICKED
        // in WM_COMMAND mirrors the click to the checkbox.
        hParalyzeCheck = CreateWindowW(
            L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            0, 0, 24, 24, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CHK_PARALYZE)), hInst, nullptr);

        hParalyzeLabel = CreateWindowW(
            L"STATIC", LoadStr(IDS_CHK_PARALYZE).c_str(),
            WS_CHILD | WS_VISIBLE | SS_NOTIFY,
            0, 0, 100, 24, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CHK_PARALYZE + 100)), hInst, nullptr);

        SendMessage(hParalyzeLabel, WM_SETFONT, reinterpret_cast<WPARAM>(hUiFont), TRUE);
        SetWindowSubclass(hParalyzeLabel, ParalyzeLabelSubclassProc, 1, 0);

        // --- Action buttons ---
        hKillOnceButton = CreateWindowW(
            L"BUTTON", LoadStr(IDS_BTN_KILL).c_str(),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, BUTTON_WIDTH, BUTTON_HEIGHT, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_KILL_PROCESS)), hInst, nullptr);

        hRestoreButton = CreateWindowW(
            L"BUTTON", LoadStr(IDS_BTN_RESTORE).c_str(),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, BUTTON_WIDTH, BUTTON_HEIGHT, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RESTORE_PROCESS)), hInst, nullptr);

        SendMessage(hKillOnceButton, WM_SETFONT, reinterpret_cast<WPARAM>(hButtonFont), TRUE);
        SendMessage(hRestoreButton,  WM_SETFONT, reinterpret_cast<WPARAM>(hButtonFont), TRUE);

        // --- Informational statics at the bottom ---
        hStatusText = CreateWindowW(
            L"STATIC", LoadStr(IDS_STATUS_READY).c_str(),
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            0, 0, 100, 25, hwnd, nullptr, hInst, nullptr);
        SendMessage(hStatusText, WM_SETFONT, reinterpret_cast<WPARAM>(hStatusFont), TRUE);

        hProcessCount = CreateWindowW(
            L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            0, 0, 100, 20, hwnd, nullptr, hInst, nullptr);
        SendMessage(hProcessCount, WM_SETFONT, reinterpret_cast<WPARAM>(hCountFont), TRUE);

        // Store the main-window handle so worker threads can PostMessage to it.
        hInstMain = hwnd;

        LayoutMainWindow(hwnd);
        RefreshProcessList();
        ApplyModernWindowEffects(hwnd);

        // Periodic list refresh at ~2 s intervals.
        SetTimer(hwnd, IDC_REFRESH_TIMER, 2000, nullptr);
        // Paralyze label uses a sine-pulse animation; ~25 fps is smooth enough.
        SetTimer(hwnd, IDC_PARALYZE_TIMER, 40, nullptr);
        break;
    }

    // -------------------------------------------------------------------------
    case WM_TIMER:
        if (wParam == IDC_REFRESH_TIMER)
            RefreshProcessList();
        // Invalidate just the label so its subclassed WM_PAINT recalculates
        // the sine-pulse color and repaints atomically each frame.
        if (wParam == IDC_PARALYZE_TIMER && hParalyzeLabel)
            InvalidateRect(hParalyzeLabel, nullptr, FALSE);
        break;

    // -------------------------------------------------------------------------
    case WM_COMMAND:
        if      (LOWORD(wParam) == IDC_KILL_PROCESS)    KillOnceAction();
        else if (LOWORD(wParam) == IDC_RESTORE_PROCESS)  RestoreAction();
        else if ((LOWORD(wParam) == IDC_SHOW_EXTRA ||
                  LOWORD(wParam) == IDC_HIDE_INACTIVE_BUILTIN) &&
                 HIWORD(wParam) == BN_CLICKED)
            RefreshProcessList();
        // Paralyze label (SS_NOTIFY STATIC) forwards clicks to the checkbox.
        else if (LOWORD(wParam) == IDC_CHK_PARALYZE + 100 && HIWORD(wParam) == STN_CLICKED) {
            bool isChecked = SendMessageW(hParalyzeCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            SendMessageW(hParalyzeCheck, BM_SETCHECK, isChecked ? BST_UNCHECKED : BST_CHECKED, 0);
        }
        break;

    // -------------------------------------------------------------------------
    // Custom message posted by worker threads to trigger a list refresh.
    case WMU_REFRESH_LIST:
        RefreshProcessList();
        break;

    // -------------------------------------------------------------------------
    // Enables or disables both action buttons.  wParam != 0 → enable.
    case WMU_SET_BUTTONS:
        EnableWindow(hKillOnceButton, wParam != 0);
        EnableWindow(hRestoreButton,  wParam != 0);
        break;

    // -------------------------------------------------------------------------
    case WMU_STATUS_TEXT: {
        // Worker thread allocated the string; we own it here and must delete it.
        auto* pText = reinterpret_cast<std::wstring*>(wParam);
        if (pText) {
            SetWindowTextW(hStatusText, pText->c_str());
            delete pText;
            InvalidateRect(hStatusText, nullptr, TRUE);
        }
        break;
    }

    // -------------------------------------------------------------------------
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintHeaderPanels(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    // -------------------------------------------------------------------------
    case WM_CTLCOLORSTATIC: {
        const auto hdcStatic  = reinterpret_cast<HDC>(wParam);
        const auto hwndStatic = reinterpret_cast<HWND>(lParam);

        if (hwndStatic == hHeaderInfoText || hwndStatic == hHeaderWarningText) {
            // Header text renders on the custom-drawn panel background.
            SetTextColor(hdcStatic, g_isDarkMode ? RGB(232, 236, 242) : RGB(40, 44, 52));
            SetBkMode(hdcStatic, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));

        } else if (hwndStatic == hStatusText) {
            SetTextColor(hdcStatic, g_isDarkMode ? RGB(116, 226, 132) : RGB(0, 128, 0));

        } else if (hwndStatic == hProcessCount) {
            SetTextColor(hdcStatic, g_isDarkMode ? RGB(128, 170, 255) : RGB(0, 0, 128));

        } else {
            SetTextColor(hdcStatic, g_isDarkMode ? RGB(228, 228, 228) : RGB(24, 24, 24));
        }

        SetBkColor(hdcStatic, g_windowColor);
        return reinterpret_cast<LRESULT>(hWindowBrush);
    }

    // -------------------------------------------------------------------------
    case WM_CTLCOLORBTN: {
        // Only handles push buttons if they are not themed; usually not needed for checkbuttons.
        const auto hdcBtn = reinterpret_cast<HDC>(wParam);
        SetBkColor(hdcBtn, g_windowColor);
        return reinterpret_cast<LRESULT>(hWindowBrush);
    }

    // -------------------------------------------------------------------------
    case WM_SIZE:
        LayoutMainWindow(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        break;

    // -------------------------------------------------------------------------
    case WM_NOTIFY: {
        const auto* hdr = reinterpret_cast<LPNMHDR>(lParam);
        if (hdr->hwndFrom == hListView) {
            if (hdr->code == NM_CUSTOMDRAW)
                return HandleListCustomDraw(lParam);
            if (hdr->code == NM_CLICK) {
                HandleListRowClick();
                return 0;
            }
            // Column header click — toggle direction or switch sort column.
            if (hdr->code == LVN_COLUMNCLICK) {
                const auto* pnmv = reinterpret_cast<LPNMLISTVIEW>(lParam);
                if (g_sortColumn == pnmv->iSubItem) g_sortAscending = !g_sortAscending;
                else { g_sortColumn = pnmv->iSubItem; g_sortAscending = true; }
                RefreshProcessList();
            }
        }
        break;
    }

    // -------------------------------------------------------------------------
    case WM_GETMINMAXINFO: {
        // Enforce a minimum window size so the layout doesn't collapse.
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = MIN_WINDOW_WIDTH;
        mmi->ptMinTrackSize.y = MIN_WINDOW_HEIGHT;
        return 0;
    }

    // -------------------------------------------------------------------------
    // Re-apply dark/light mode whenever the system theme changes.
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        ApplyModernWindowEffects(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        break;

    // -------------------------------------------------------------------------
    case WM_CLOSE:
        g_shutdownPending = true;
        if (g_activeWorkers.load() == 0)
            DestroyWindow(hwnd);
        // If workers are still running, the last one to finish will re-post
        // WM_CLOSE, which will then see g_activeWorkers == 0 and call
        // DestroyWindow — so the window never closes mid-operation.
        break;

    // -------------------------------------------------------------------------
    case WM_DESTROY:
        KillTimer(hwnd, IDC_REFRESH_TIMER);
        KillTimer(hwnd, IDC_PARALYZE_TIMER);
        // g_statusLock is intentionally NOT deleted here: a worker thread that
        // was already past the g_shutdownPending check might still hold it.
        // The OS reclaims all process resources on exit.
        DeleteUiObject(hUiFont);
        DeleteUiObject(hButtonFont);
        DeleteUiObject(hStatusFont);
        DeleteUiObject(hCountFont);
        DeleteUiObject(hWindowBrush);
        PostQuitMessage(0);
        break;

    // -------------------------------------------------------------------------
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    return 0;
}

// =============================================================================
// Entry Point
// =============================================================================

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/,
                   LPSTR /*lpCmdLine*/, int nCmdShow)
{
    // If any arguments were passed, delegate to the CLI handler and exit.
    // This allows the binary to be used headlessly (e.g., from scripts).
    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 1) {
        const int result = RunCLI(argc, argv);
        LocalFree(argv);
        return result;
    }
    if (argv) LocalFree(argv);

    // ------------------------------------------------------------------
    // GUI mode initialisation
    // ------------------------------------------------------------------

    // Detect system dark mode preference before any UI is created so
    // the initial window background brush has the correct color.
    g_isDarkMode  = AppUseDarkMode();
    g_windowColor = g_isDarkMode ? RGB(40, 40, 40) : RGB(245, 245, 245);
    hInst         = hInstance;
    hWindowBrush  = CreateSolidBrush(g_windowColor);

    // Register the ListView common control class.
    INITCOMMONCONTROLSEX icex{ sizeof(INITCOMMONCONTROLSEX), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icex);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hbrBackground = hWindowBrush;
    wc.lpszClassName = L"KVCKILLER";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(MAIN_ICON));
    // 16×16 small icon used in the taskbar and Alt+Tab.
    wc.hIconSm       = static_cast<HICON>(
        LoadImageW(hInstance, MAKEINTRESOURCEW(MAIN_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, LoadStr(IDS_ERR_WINDOW_CLASS).c_str(),
                    LoadStr(IDS_ERR_CAPTION).c_str(), MB_ICONERROR);
        return 0;
    }

    // Center the initial window on the primary monitor.
    const int screenWidth  = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    HWND hwnd = CreateWindowW(
        L"KVCKILLER", LoadStr(IDS_WINDOW_TITLE).c_str(),
        WS_OVERLAPPEDWINDOW,
        (screenWidth  - WINDOW_WIDTH)  / 2,
        (screenHeight - WINDOW_HEIGHT) / 2,
        WINDOW_WIDTH, WINDOW_HEIGHT,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) {
        MessageBoxW(nullptr, LoadStr(IDS_ERR_WINDOW_CREATE).c_str(),
                    LoadStr(IDS_ERR_CAPTION).c_str(), MB_ICONERROR);
        return 0;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Standard Win32 message loop.
    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return static_cast<int>(msg.wParam);
}
