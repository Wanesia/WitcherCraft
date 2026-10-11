// Offline verifier: maps witcher3.exe as an image resource (nothing executes)
// and runs the exact runtime resolver from red_engine.cpp against it.
//
//   wcraft_scan.exe ["C:\...\bin\x64_dx12\witcher3.exe"]
//
// Exit code 0 = all signatures resolved.
#include "log.hpp"
#include "pe.hpp"
#include "red_engine.hpp"

#include <Windows.h>
#include <cstdio>

int wmain(int argc, wchar_t** argv)
{
    const wchar_t* path = argc > 1 ? argv[1]
                                   : L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\The Witcher 3\\bin\\x64_dx12\\witcher3.exe";
    wc::log::InitConsole();

    HMODULE h = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!h)
    {
        std::fwprintf(stderr, L"LoadLibraryEx(%s) failed: %lu\n", path, GetLastError());
        return 2;
    }
    // Image-resource mappings carry tag bits in the low bits of the handle.
    auto* base = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(h) & ~static_cast<uintptr_t>(0xFFFF));

    wc::pe::Module m;
    if (!m.Init(base))
    {
        std::fprintf(stderr, "not a PE64 image\n");
        return 2;
    }
    WLOG("image %ls", path);
    WLOG("TimeDateStamp 0x%08X SizeOfImage 0x%zX sections %zu", m.timeDateStamp, m.imageSize, m.sections.size());

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    wc::red::Offsets o;
    const bool ok = wc::red::Resolve(m, o);
    QueryPerformanceCounter(&t1);
    WLOG("Resolve: %s in %.1f ms", ok ? "OK" : "FAILED", (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    wc::red::DumpOffsets(m, o);

    const int mism = wc::red::CompareWithReference(m, o);
    if (mism < 0)
        WLOG("reference check: different build than 5.0.0.1044392 (0x6ABD8695) - nothing to compare");
    else
        WLOG("reference check: %d mismatch(es) vs. the reference RVAs", mism);

    FreeLibrary(h);
    return ok && mism <= 0 ? 0 : 1;
}
