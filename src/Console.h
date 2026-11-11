#pragma once

#include <windows.h>
#include <string_view>

namespace Console {

void Setup();
void SendEnter();
bool IsConsoleAttached();

void Print(std::wstring_view text);
void PrintLine(std::wstring_view text = L"");
void PrintFormat(const wchar_t* format, ...);
void PrintLineFormat(const wchar_t* format, ...);

} // namespace Console
