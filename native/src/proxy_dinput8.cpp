// dinput8.dll proxy: witcher3.exe 5.0 statically imports DINPUT8.dll!DirectInput8Create,
// and dinput8 is not a KnownDLL, so a dinput8.dll placed next to witcher3.exe is
// loaded (and its DllMain runs) before the game's own code starts.
// Every export forwards to %SystemRoot%\System32\dinput8.dll, loaded lazily
// (never from DllMain).
#include <Windows.h>
#include <Unknwn.h>

namespace
{
INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
HMODULE g_real = nullptr;

BOOL CALLBACK LoadReal(PINIT_ONCE, PVOID, PVOID*)
{
    wchar_t path[MAX_PATH]{};
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n > MAX_PATH - 14)
        return TRUE;
    wcscat_s(path, L"\\dinput8.dll");
    g_real = LoadLibraryW(path);
    return TRUE;
}

FARPROC Real(const char* name)
{
    InitOnceExecuteOnce(&g_once, &LoadReal, nullptr, nullptr);
    return g_real ? GetProcAddress(g_real, name) : nullptr;
}

template <class T> T RealAs(const char* name)
{
    return reinterpret_cast<T>(Real(name));
}
} // namespace

extern "C" HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, REFIID riid, LPVOID* ppvOut,
                                                   LPUNKNOWN punkOuter)
{
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto fn = RealAs<Fn>("DirectInput8Create");
    return fn ? fn(hinst, dwVersion, riid, ppvOut, punkOuter) : E_FAIL;
}

extern "C" HRESULT WINAPI Proxy_DllCanUnloadNow()
{
    using Fn = HRESULT(WINAPI*)();
    auto fn = RealAs<Fn>("DllCanUnloadNow");
    return fn ? fn() : S_FALSE;
}

extern "C" HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
{
    using Fn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    auto fn = RealAs<Fn>("DllGetClassObject");
    return fn ? fn(rclsid, riid, ppv) : CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" HRESULT WINAPI Proxy_DllRegisterServer()
{
    using Fn = HRESULT(WINAPI*)();
    auto fn = RealAs<Fn>("DllRegisterServer");
    return fn ? fn() : E_FAIL;
}

extern "C" HRESULT WINAPI Proxy_DllUnregisterServer()
{
    using Fn = HRESULT(WINAPI*)();
    auto fn = RealAs<Fn>("DllUnregisterServer");
    return fn ? fn() : E_FAIL;
}

extern "C" const void* WINAPI Proxy_GetdfDIJoystick()
{
    using Fn = const void*(WINAPI*)();
    auto fn = RealAs<Fn>("GetdfDIJoystick");
    return fn ? fn() : nullptr;
}
