// WitcherCraft native bridge for The Witcher 3 patch 5.0.
//
// Load path: dinput8.dll proxy in bin\x64_dx12 (or witchercraft.asi via an ASI loader).
// DllMain runs during static-import initialisation, i.e. BEFORE witcher3.exe's own
// code. We only scan memory and patch one rel32 call here (no LoadLibrary, no waits), except on a
// release install's first start, where the Minecraft setup shows a message box, unpacks Prism and
// closes the game before any of its code runs (launcher::FirstStartSetup).
#include "config.hpp"
#include "launcher.hpp"
#include "log.hpp"
#include "log_channel.hpp"
#include "natives.hpp"
#include "overlay_dx12.hpp"
#include "pe.hpp"
#include "red_engine.hpp"

#include <Windows.h>
#include <cwchar>

namespace
{
bool IsWitcherProcess()
{
    wchar_t path[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"witcher3.exe") == 0;
}

bool AlreadyLoaded()
{
    // Guards against dinput8.dll + witchercraft.asi both being present.
    wchar_t buf[8]{};
    if (GetEnvironmentVariableW(L"WITCHERCRAFT_NATIVE_ACTIVE", buf, 8) > 0)
        return true;
    SetEnvironmentVariableW(L"WITCHERCRAFT_NATIVE_ACTIVE", L"1");
    return false;
}

void Init(HMODULE self)
{
    if (!IsWitcherProcess() || AlreadyLoaded())
        return;

    wc::log::Init(self);
    wchar_t exe[MAX_PATH * 2]{}, me[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    GetModuleFileNameW(self, me, MAX_PATH * 2);
    WLOG("WitcherCraft native bridge %s starting", WCRAFT_VERSION);
    WLOG("host: %ls", exe);
    WLOG("self: %ls", me);
    WLOG("log : %ls", wc::log::Path());

    wc::LoadConfig(self);
    if (wc::launcher::FirstStartSetup(self))
    {
        WLOG("closing the game for the first-time Minecraft setup");
        TerminateProcess(GetCurrentProcess(), 0);
    }
    wc::overlay::Start(); // HUD overlay: only spawns a thread here (hooks are installed off DllMain)

    wc::pe::Module m;
    if (!m.Init(GetModuleHandleW(nullptr)))
    {
        WLOG("cannot parse host PE headers - doing nothing");
        return;
    }
    WLOG("witcher3.exe base %p, TimeDateStamp 0x%08X, SizeOfImage 0x%zX", m.base, m.timeDateStamp, m.imageSize);

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    const bool ok = wc::red::Resolve(m, wc::red::g);
    QueryPerformanceCounter(&t1);
    WLOG("signature scan %s in %.1f ms", ok ? "OK" : "FAILED", (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    wc::red::DumpOffsets(m, wc::red::g);
    const int mism = wc::red::CompareWithReference(m, wc::red::g);
    if (mism >= 0)
        WLOG("reference build 5.0.0.1044392: %d mismatch(es)", mism);
    else
        WLOG("NOTE: not the reference 5.0.0.1044392 build; relying on signatures only");

    if (!ok)
    {
        WLOG("scripting bridge DISABLED (scripts importing WCraft_* will fail to compile)");
        return;
    }

    wc::launcher::Start(self); // bundled Minecraft (release installs): unpack and start it, off this thread

    if (wc::g_config.registerNatives)
    {
        if (!wc::natives::InstallRegistrationHook())
            WLOG("registration hook FAILED");
    }

    if (wc::g_config.logChannelFallback)
    {
        if (wc::logchannel::Start())
            WLOG("log-channel fallback armed (Log(\"WCRAFT:<cmd>\") from scripts)");
        else
            WLOG("log-channel fallback unavailable");
    }
}
} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        Init(hModule);
    }
    return TRUE;
}
