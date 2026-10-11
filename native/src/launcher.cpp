// Minecraft starts with The Witcher 3 (see launcher.hpp). Ported from SkyCraft's skse/src/Launcher.cpp
// (MIT, chasmlol): the bundle unpacking and the "a Minecraft is already running" check are the same.
// Prism is started directly instead of through Explorer (see Run), and SkyCraft's fallback to an
// installed Prism is left out (on a dev PC it would start the developer's own Prism with an instance
// that isn't there).
#include "launcher.hpp"

#include "config.hpp"
#include "log.hpp"
#include "skycraft_protocol.h"

#include <TlHelp32.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
namespace proto = skycraft::proto;

namespace wc::launcher
{
namespace
{
constexpr wchar_t kSection[] = L"Minecraft";
constexpr wchar_t kDefaultArgs[] = L"--launch WitcherCraft";

fs::path g_bundle; // <plugin dir>\witchercraft\WitcherCraft-Minecraft.zip

std::wstring ExpandEnv(const std::wstring& path)
{
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = ExpandEnvironmentStringsW(path.c_str(), buf, static_cast<DWORD>(std::size(buf)));
    return n > 0 && n <= std::size(buf) ? std::wstring(buf) : path;
}

std::wstring IniString(const wchar_t* key, const wchar_t* def)
{
    wchar_t v[MAX_PATH * 2]{};
    GetPrivateProfileStringW(kSection, key, def, v, static_cast<DWORD>(std::size(v)), g_config.iniPath);
    return v;
}

fs::path InstallDir() { return ExpandEnv(L"%LOCALAPPDATA%\\WitcherCraft"); }

// True while this Prism (exe path) is running. Prism allows one copy of itself: a "--launch" sent while
// the last one is still closing (it quits right after its Minecraft) goes to that one and is lost.
bool PrismRunning(const fs::path& exe)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W entry{sizeof(entry)};
    bool found = false;
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry))
    {
        if (_wcsicmp(entry.szExeFile, L"prismlauncher.exe") != 0)
            continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process)
            continue;
        wchar_t path[MAX_PATH * 2]{};
        DWORD n = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(process, 0, path, &n))
            found = _wcsicmp(path, exe.c_str()) == 0;
        CloseHandle(process);
    }
    CloseHandle(snapshot);
    return found;
}

// A Minecraft with the WitcherCraft mod holds this mutex while it runs (SkyLink.announceRunning).
bool MinecraftRunning()
{
    const std::wstring name = std::wstring(proto::kMappingName) + L"_minecraft";
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, name.c_str());
    if (mutex)
    {
        CloseHandle(mutex);
        return true;
    }
    return false;
}


// Unpacks the bundle to %LOCALAPPDATA%\WitcherCraft the first time, and again whenever this release
// brings a different one. Prism's own data there (the signed-in account, the downloaded Minecraft and
// Java, the WitcherCraft world) is kept; the instance and its WitcherCraft and Fabric API jars are
// replaced, so both halves always match. Returns Prism's exe, or empty on failure.
fs::path EnsureBundle()
{
    const fs::path dir = InstallDir();
    const fs::path prism = dir / "Prism" / "prismlauncher.exe";
    std::error_code ec;
    const auto size = fs::file_size(g_bundle, ec);
    const auto time = fs::last_write_time(g_bundle, ec).time_since_epoch().count();
    const std::string stamp = std::to_string(size) + " " + std::to_string(time);
    std::string installed;
    if (std::ifstream in{dir / "bundle.stamp"}; in)
        std::getline(in, installed);
    if (installed == stamp && fs::exists(prism))
        return prism;

    WLOG("minecraft: unpacking the bundled Minecraft to %ls", dir.c_str());
    fs::create_directories(dir, ec);
    for (const auto& entry : fs::directory_iterator(dir / "Prism" / "instances" / "WitcherCraft" / ".minecraft" / "mods", ec))
    {
        const std::wstring name = entry.path().filename().wstring();
        if (name.starts_with(L"witchercraft-") || name.starts_with(L"fabric-api-"))
            fs::remove(entry.path(), ec);
    }
    // A real copy first (the game may see the bundle through a mod manager's links or virtual files).
    const fs::path copy = dir / "bundle.zip";
    if (!fs::copy_file(g_bundle, copy, fs::copy_options::overwrite_existing, ec))
    {
        WLOG("minecraft: couldn't copy %ls (%hs)", g_bundle.c_str(), ec.message().c_str());
        return {};
    }
    // Relative paths only: tar.exe reads its arguments in the ANSI code page, so a folder name outside it
    // (a user name like "Paweł" on English Windows) can't be passed. The working directory is set
    // through CreateProcessW, which takes any name.
    std::wstring command = L"\"" + ExpandEnv(L"%SystemRoot%\\System32\\tar.exe") + L"\" -xf bundle.zip";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    DWORD code = 1;
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi))
    {
        WaitForSingleObject(pi.hProcess, 5 * 60 * 1000);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    fs::remove(copy, ec);
    if (code != 0 || !fs::exists(prism))
    {
        WLOG("minecraft: unpacking failed (tar exit code %lu)", code);
        return {};
    }
    // Prism's settings: only the first time (after that they're the player's).
    const fs::path cfg = dir / "Prism" / "prismlauncher.cfg";
    if (!fs::exists(cfg))
        fs::copy_file(dir / "defaults" / "prismlauncher.cfg", cfg, ec);
    std::ofstream(dir / "bundle.stamp") << stamp;
    return prism;
}

// Starts a program as its own process (not tied to the game: no inherited handles, its own process
// group, out of the game's job when Windows allows it). Batch files go through cmd, without a window.
// (SkyCraft asks Explorer to start it through the shell's COM interfaces, for Mod Organizer's virtual
// files; that loads the shell and its extensions into the game, which The Witcher 3 doesn't need.)
bool Run(const fs::path& program, const std::wstring& args)
{
    const std::wstring ext = program.extension().wstring();
    const bool script = _wcsicmp(ext.c_str(), L".bat") == 0 || _wcsicmp(ext.c_str(), L".cmd") == 0;
    const std::wstring dir = program.parent_path().wstring();
    std::wstring command = script ? L"cmd.exe /c \"\"" + program.wstring() + L"\" " + args + L"\"" : L"\"" + program.wstring() + L"\" " + args;
    const DWORD flags = CREATE_NEW_PROCESS_GROUP | (script ? CREATE_NO_WINDOW : 0);
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, flags | CREATE_BREAKAWAY_FROM_JOB, nullptr, dir.c_str(), &si, &pi);
    if (!ok && GetLastError() == ERROR_ACCESS_DENIED) // the game's job doesn't allow breaking away
        ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, flags, nullptr, dir.c_str(), &si, &pi);
    if (!ok)
    {
        WLOG("minecraft: couldn't start %ls (error %lu)", program.c_str(), GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    WLOG("minecraft: started %ls %ls", program.c_str(), args.c_str());
    return true;
}

void StartNow()
{
    if (MinecraftRunning())
    {
        WLOG("minecraft: already running");
        // It may be the last game's Minecraft on its way out (it quits a few seconds after that game
        // closes): if it goes within the next minute, start one for this game.
        for (int i = 0; i < 60; ++i)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!MinecraftRunning())
            {
                WLOG("minecraft: the one that was running has quit; starting another");
                std::this_thread::sleep_for(std::chrono::seconds(3)); // let its launcher close too
                StartNow();
                return;
            }
        }
        return;
    }
    const fs::path chosen = ExpandEnv(IniString(L"Launcher", L""));
    if (!chosen.empty())
    {
        if (!fs::exists(chosen))
        {
            WLOG("minecraft: not started: %ls doesn't exist (Launcher= in witchercraft_native.ini)", chosen.c_str());
            return;
        }
        Run(chosen, IniString(L"Arguments", kDefaultArgs));
        return;
    }
    // The last game's Prism may still be closing: wait for it before unpacking over its files (a new
    // release replaces them) and before starting it again.
    const fs::path running = InstallDir() / "Prism" / "prismlauncher.exe";
    for (int i = 0; i < 40 && PrismRunning(running); ++i)
    {
        if (i == 0)
            WLOG("minecraft: waiting for the last Prism Launcher to close");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    const fs::path prism = EnsureBundle();
    if (prism.empty())
        return;
    if (!fs::exists(prism.parent_path() / "accounts.json"))
        WLOG("minecraft: first start: Prism Launcher will ask to sign in with the Microsoft account that owns Minecraft");
    Run(prism, kDefaultArgs);
}

// A release install writes witchercraft_native.ini the first time, with the settings a player may want
// (their own launcher), so there's nothing to copy or rename. The zip doesn't carry it, so installing
// an update never replaces the player's copy. UTF-16 with a BOM: GetPrivateProfileStringW then reads a
// launcher path in any language, and Notepad keeps the encoding when saving.
void WriteDefaultIni()
{
    std::error_code ec;
    if (!g_config.iniPath[0] || fs::exists(g_config.iniPath, ec))
        return;
    static constexpr wchar_t kText[] =
        L"\xFEFF"
        L"; WitcherCraft settings. You only need these to use your own Minecraft launcher.\r\n"
        L"; WitcherCraft made this file the first time it started. Updates never replace it.\r\n"
        L"; Delete it to go back to the defaults.\r\n"
        L"\r\n"
        L"[Minecraft]\r\n"
        L"; 1 = Minecraft starts and stops with The Witcher 3. 0 = you start Minecraft yourself.\r\n"
        L"StartWithWitcher=1\r\n"
        L"\r\n"
        L"; Empty = the Minecraft that comes with WitcherCraft.\r\n"
        L"; To use your own launcher instead, put its full path here, for example:\r\n"
        L";   Launcher=C:\\Games\\PrismLauncher\\prismlauncher.exe\r\n"
        L"Launcher=\r\n"
        L"\r\n"
        L"; What WitcherCraft passes to your own launcher. For Prism Launcher and MultiMC:\r\n"
        L"; --launch followed by the name of your WitcherCraft instance.\r\n"
        L"Arguments=--launch WitcherCraft\r\n";
    std::ofstream out(fs::path(g_config.iniPath), std::ios::binary);
    out.write(reinterpret_cast<const char*>(kText), sizeof(kText) - sizeof(wchar_t));
    if (out.good())
        WLOG("minecraft: wrote the default settings to %ls", g_config.iniPath);
    else
        WLOG("minecraft: couldn't write %ls (the defaults apply)", g_config.iniPath);
}
// The bundle next to the plugin, the default ini (release installs), and this game's process id in the
// environment the launcher inherits: Prism passes it on to Minecraft, which quits within seconds if
// this game is gone before it ever connected (SkyClient.quitWithSkyrim).
void Prepare(HMODULE self)
{
    if (!g_bundle.empty())
        return;
    wchar_t me[MAX_PATH * 2]{};
    GetModuleFileNameW(self, me, MAX_PATH * 2);
    g_bundle = fs::path(me).parent_path() / "witchercraft" / "WitcherCraft-Minecraft.zip";
    std::error_code ec;
    if (fs::exists(g_bundle, ec))
        WriteDefaultIni();
    SetEnvironmentVariableW(L"WITCHERCRAFT_GAME_PID", std::to_wstring(GetCurrentProcessId()).c_str());
}

constexpr wchar_t kSetupTitle[] = L"WitcherCraft";
constexpr wchar_t kSetupText[] =
    L"WitcherCraft needs to set up Minecraft once before you can play.\n\n"
    L"After you click OK:\n"
    L"1. Prism Launcher opens and asks you to add a Microsoft account. Add the account that owns Minecraft.\n"
    L"2. Prism downloads Minecraft. This takes a few minutes.\n"
    L"3. When the Prism window closes by itself, start The Witcher 3 again.\n\n"
    L"The Witcher 3 closes now. This only happens the first time.";
constexpr wchar_t kSetupBusyText[] =
    L"Minecraft is still being set up.\n\n"
    L"Finish adding your Microsoft account in the Prism Launcher window and wait until it closes by "
    L"itself. Then start The Witcher 3 again.";
} // namespace

bool FirstStartSetup(HMODULE self)
{
    Prepare(self);
    std::error_code ec;
    if (!fs::exists(g_bundle, ec) || !IniString(L"Launcher", L"").empty() ||
        GetPrivateProfileIntW(kSection, L"StartWithWitcher", 1, g_config.iniPath) == 0)
        return false;
    const fs::path prism = InstallDir() / "Prism" / "prismlauncher.exe";
    if (fs::exists(prism.parent_path() / "accounts.json", ec))
        return false;

    if (PrismRunning(prism))
    {
        WLOG("minecraft: first-time setup still running in Prism Launcher; closing the game");
        MessageBoxW(nullptr, kSetupBusyText, kSetupTitle, MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
        return true;
    }
    WLOG("minecraft: first start: setting Minecraft up without the game (sign-in and download in Prism Launcher)");
    MessageBoxW(nullptr, kSetupText, kSetupTitle, MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
    const fs::path unpacked = EnsureBundle();
    if (unpacked.empty())
        return false; // play without Minecraft; the log says why
    return Run(unpacked, kDefaultArgs);
}

void Start(HMODULE self)
{
    Prepare(self);

    if (GetPrivateProfileIntW(kSection, L"StartWithWitcher", 1, g_config.iniPath) == 0)
    {
        WLOG("minecraft: not started with the game (StartWithWitcher=0)");
        return;
    }
    std::error_code ec;
    if (IniString(L"Launcher", L"").empty() && !fs::exists(g_bundle, ec))
    {
        WLOG("minecraft: no bundled Minecraft (%ls) and no Launcher= set: start Minecraft yourself", g_bundle.c_str());
        return;
    }
    // Off the loader lock and the game's start-up: unpacking takes a moment.
    std::thread(StartNow).detach();
}
} // namespace wc::launcher
