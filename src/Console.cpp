#include "Console.h"
#include "StrUtils.h"
#include <cstdarg>

namespace Console {

namespace {

struct OutputTarget {
    HANDLE handle     = nullptr;
    bool   isConsole  = false;
    bool   isAttached = false;
    bool   inited     = false;
};

OutputTarget g_out;

void EnsureTarget() noexcept {
    if (g_out.inited) return;

    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) {
        DWORD ft = GetFileType(h);
        if (ft != FILE_TYPE_UNKNOWN) {
            g_out.handle = h;
            DWORD mode = 0;
            g_out.isConsole = GetConsoleMode(h, &mode);
            g_out.inited = true;
            return;
        }
    }

    g_out.handle = CreateFileW(L"CONOUT$", GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    if (g_out.handle == INVALID_HANDLE_VALUE) {
        g_out.handle = nullptr;
    }

    DWORD mode = 0;
    g_out.isConsole = (g_out.handle != nullptr && GetConsoleMode(g_out.handle, &mode));
    g_out.inited = true;
}

} // namespace

void Setup() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) {
        DWORD ft = GetFileType(h);
        if (ft == FILE_TYPE_DISK || ft == FILE_TYPE_PIPE) {
            g_out.handle = h;
            g_out.isConsole = false;
            g_out.isAttached = false;
            g_out.inited = true;
            return;
        }
    }

    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        g_out.isAttached = true;
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
    }
    EnsureTarget();

    if (g_out.isAttached && g_out.handle) {
        CONSOLE_SCREEN_BUFFER_INFO csbi{};
        if (GetConsoleScreenBufferInfo(g_out.handle, &csbi)) {
            if (csbi.dwCursorPosition.X > 0) {
                Print(L"\r\n");
            }
        } else {
            Print(L"\r\n");
        }
    }
}

bool IsConsoleAttached() {
    return g_out.isAttached;
}

void SendEnter() {
    if (!g_out.isConsole && !g_out.isAttached) return;

    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    if (hStdin == INVALID_HANDLE_VALUE || !hStdin) return;
    DWORD mode = 0;
    if (!GetConsoleMode(hStdin, &mode)) return;

    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (g_out.handle && GetConsoleScreenBufferInfo(g_out.handle, &csbi)) {
        if (csbi.dwCursorPosition.X == 0 && csbi.dwCursorPosition.Y > 0) {
            COORD pos = { 0, static_cast<SHORT>(csbi.dwCursorPosition.Y - 1) };
            SetConsoleCursorPosition(g_out.handle, pos);
        }
    }

    FlushConsoleInputBuffer(hStdin);

    INPUT_RECORD ir[2] = {};
    ir[0].EventType                        = KEY_EVENT;
    ir[0].Event.KeyEvent.bKeyDown          = TRUE;
    ir[0].Event.KeyEvent.wRepeatCount      = 1;
    ir[0].Event.KeyEvent.wVirtualKeyCode   = VK_RETURN;
    ir[0].Event.KeyEvent.wVirtualScanCode  = static_cast<WORD>(MapVirtualKeyW(VK_RETURN, MAPVK_VK_TO_VSC));
    ir[0].Event.KeyEvent.uChar.UnicodeChar = L'\r';
    ir[0].Event.KeyEvent.dwControlKeyState = 0;

    ir[1] = ir[0];
    ir[1].Event.KeyEvent.bKeyDown = FALSE;

    DWORD written = 0;
    WriteConsoleInputW(hStdin, ir, 2, &written);
}

void Print(std::wstring_view text) {
    if (text.empty()) return;
    EnsureTarget();
    if (!g_out.handle) return;

    DWORD written = 0;
    if (g_out.isConsole) {
        WriteConsoleW(g_out.handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    } else {
        constexpr size_t kChunk = 512;
        char utf8[kChunk * 3];
        const wchar_t* p = text.data();
        size_t remaining = text.size();

        while (remaining > 0) {
            size_t count = (remaining < kChunk) ? remaining : kChunk;
            if (count < remaining && IS_HIGH_SURROGATE(p[count - 1])) {
                --count;
            }
            int bytes = WideCharToMultiByte(CP_UTF8, 0, p, static_cast<int>(count),
                                            utf8, static_cast<int>(sizeof(utf8)), nullptr, nullptr);
            if (bytes > 0) {
                WriteFile(g_out.handle, utf8, static_cast<DWORD>(bytes), &written, nullptr);
            }
            p += count;
            remaining -= count;
        }
    }
}

void PrintLine(std::wstring_view text) {
    if (!text.empty()) {
        Print(text);
    }
    Print(L"\r\n");
}

void PrintFormat(const wchar_t* format, ...) {
    if (!format) return;
    wchar_t buffer[1024];
    va_list args;
    va_start(args, format);
    wvsprintfW(buffer, format, args);
    va_end(args);
    Print(buffer);
}

void PrintLineFormat(const wchar_t* format, ...) {
    if (!format) return;
    wchar_t buffer[1024];
    va_list args;
    va_start(args, format);
    wvsprintfW(buffer, format, args);
    va_end(args);
    PrintLine(buffer);
}

} // namespace Console
