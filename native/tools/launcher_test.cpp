// Offline test of the bundled-Minecraft launcher (src/launcher.cpp), without the game:
//   launcher_test.exe <dir>
// Runs wc::launcher::Start exactly as the plugin does, with %LOCALAPPDATA% pointed at <dir> (so the
// bundle unpacks to <dir>\WitcherCraft, not the real one) and the bundle expected next to this exe
// (witchercraft\WitcherCraft-Minecraft.zip). Waits up to 120 s for <dir>\WitcherCraft\Prism\
// prismlauncher.exe to be running (a new one, not one already there). Exit 0 = Prism started (left
// running: close it yourself).
#include "config.hpp"
#include "launcher.hpp"
#include "log.hpp"

#include <TlHelp32.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <set>
#include <thread>

namespace
{
// A Prism running from `exe` whose pid isn't in `ignore` (one still closing from an earlier run).
bool PrismRunningFrom(const std::wstring& exe, const std::set<DWORD>& ignore, std::set<DWORD>* all = nullptr)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W entry{sizeof(entry)};
    bool found = false;
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry))
    {
        if (_wcsicmp(entry.szExeFile, L"prismlauncher.exe") != 0 || (!all && ignore.count(entry.th32ProcessID)))
            continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process)
            continue;
        wchar_t path[MAX_PATH * 2]{};
        DWORD n = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(process, 0, path, &n) && _wcsicmp(path, exe.c_str()) == 0)
        {
            if (all)
                all->insert(entry.th32ProcessID);
            else
                found = true;
        }
        CloseHandle(process);
    }
    CloseHandle(snapshot);
    return found;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        std::fwprintf(stderr, L"usage: launcher_test.exe <dir used as %%LOCALAPPDATA%%>\n");
        return 2;
    }
    wc::log::InitConsole();
    wchar_t full[MAX_PATH * 2]{};
    GetFullPathNameW(argv[1], MAX_PATH * 2, full, nullptr);
    const std::wstring dir = full;
    SetEnvironmentVariableW(L"LOCALAPPDATA", dir.c_str());
    swprintf_s(wc::g_config.iniPath, L"%s\\witchercraft_native.ini", dir.c_str()); // absent: defaults

    const std::wstring prism = dir + L"\\WitcherCraft\\Prism\\prismlauncher.exe";
    std::set<DWORD> before; // a Prism already running (e.g. still closing) doesn't count
    PrismRunningFrom(prism, {}, &before);

    wc::launcher::Start(nullptr);

    for (int i = 0; i < 240; ++i)
    {
        if (PrismRunningFrom(prism, before))
        {
            WLOG("launcher test: PASS (Prism running from %ls after %.1f s)", prism.c_str(), i * 0.5);
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    WLOG("launcher test: FAIL (no Prism running from %ls after 120 s)", prism.c_str());
    return 1;
}
