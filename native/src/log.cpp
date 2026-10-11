#include "log.hpp"

#include <clocale>
#include <cstdarg>
#include <cstdio>
#include <cwchar>

namespace wc::log
{
namespace
{
CRITICAL_SECTION g_cs;
bool g_csInit = false;
HANDLE g_file = INVALID_HANDLE_VALUE;
bool g_console = false;
wchar_t g_path[MAX_PATH * 2]{};

HANDLE OpenLog(const wchar_t* path)
{
    return CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void EnsureCs()
{
    if (!g_csInit)
    {
        InitializeCriticalSection(&g_cs);
        g_csInit = true;
    }
}
} // namespace

void Init(HMODULE self)
{
    EnsureCs();

    wchar_t dir[MAX_PATH * 2]{};
    DWORD n = GetModuleFileNameW(self, dir, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2)
    {
        wchar_t* slash = wcsrchr(dir, L'\\');
        if (slash)
        {
            *(slash + 1) = 0;
            swprintf_s(g_path, L"%switchercraft_native.log", dir);
            g_file = OpenLog(g_path);
        }
    }

    if (g_file == INVALID_HANDLE_VALUE)
    {
        wchar_t tmp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tmp);
        swprintf_s(g_path, L"%switchercraft_native.log", tmp);
        g_file = OpenLog(g_path);
    }
}

void InitConsole()
{
    EnsureCs();
    g_console = true;
    wcscpy_s(g_path, L"<stdout>");
}

const wchar_t* Path()
{
    return g_path;
}

void Write(const char* fmt, ...)
{
    if (!g_csInit)
        return;

    // %ls arguments (paths) are written as UTF-8 with a locale of our own (the process-wide one is the
    // game's). With the default "C" locale a path outside ASCII (a user name like "Paweł") made the whole
    // line disappear.
    static const _locale_t utf8 = _create_locale(LC_ALL, ".UTF-8");
    char msg[2048]{};
    va_list ap;
    va_start(ap, fmt);
    int len = utf8 ? _vsnprintf_l(msg, sizeof(msg) - 2, fmt, utf8, ap) : vsnprintf(msg, sizeof(msg) - 2, fmt, ap);
    va_end(ap);
    if (len < 0)
        len = static_cast<int>(strnlen(msg, sizeof(msg) - 2)); // too long, or a conversion failed: keep what was written
    if (len == 0)
        len = _snprintf_s(msg, sizeof(msg) - 2, _TRUNCATE, "(unprintable log line: %s)", fmt);
    if (len < 0)
        return;
    if (len > static_cast<int>(sizeof(msg) - 2))
        len = static_cast<int>(sizeof(msg) - 2);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[2200];
    int ll = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%02u:%02u:%02u.%03u][T%05lu] %.*s\r\n", st.wHour, st.wMinute,
                         st.wSecond, st.wMilliseconds, GetCurrentThreadId(), len, msg);
    if (ll < 0)
        ll = static_cast<int>(strnlen(line, sizeof(line)));

    EnterCriticalSection(&g_cs);
    if (g_console)
    {
        fwrite(line, 1, static_cast<size_t>(ll), stdout);
        fflush(stdout);
    }
    else if (g_file != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        // No FlushFileBuffers: it waits for the disk on whatever game thread logged. The data is in
        // Windows' file cache once WriteFile returns, which survives a crash of the game.
        WriteFile(g_file, line, static_cast<DWORD>(ll), &written, nullptr);
    }
    LeaveCriticalSection(&g_cs);
}
} // namespace wc::log
