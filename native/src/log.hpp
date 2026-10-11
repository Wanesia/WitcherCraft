#pragma once
#include <Windows.h>

namespace wc::log
{
// Opens <dir of this DLL>\witchercraft_native.log (falls back to %TEMP%).
void Init(HMODULE self);
// Use a console/stdout sink instead of a file (offline tools).
void InitConsole();
void Write(const char* fmt, ...);
const wchar_t* Path();
} // namespace wc::log

#define WLOG(...) ::wc::log::Write(__VA_ARGS__)
