// M5a offline verifier: maps witcher3.exe and PhysX3Common_x64.dll as image resources (nothing
// executes) and runs the W3 collision resolver of src/collision_physx.cpp against them.
//
//   col_scan.exe ["C:\...\bin\x64_dx12"]
//
// Exit code 0 = every signature resolved and every PhysX3Common export found.
#include "collision.hpp"
#include "log.hpp"
#include "pe.hpp"

#include <Windows.h>
#include <cstdio>
#include <string>

namespace
{
uint8_t* MapImage(const std::wstring& path, HMODULE& h)
{
    h = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!h)
    {
        std::fwprintf(stderr, L"LoadLibraryEx(%s) failed: %lu\n", path.c_str(), GetLastError());
        return nullptr;
    }
    return reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(h) & ~static_cast<uintptr_t>(0xFFFF));
}

// The export table of an image mapping (same lookup the DLL does on the live module).
bool HasExport(uint8_t* base, const char* name)
{
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    auto* ex = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    auto* names = reinterpret_cast<uint32_t*>(base + ex->AddressOfNames);
    for (uint32_t i = 0; i < ex->NumberOfNames; ++i)
        if (std::strcmp(reinterpret_cast<const char*>(base + names[i]), name) == 0)
            return true;
    return false;
}
} // namespace

// Also reachable as `fake_host.exe colscan [dir]` (same code).
int RunColScan(const wchar_t* dirArg)
{
    const std::wstring dir = dirArg ? dirArg : L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\The Witcher 3\\bin\\x64_dx12";

    HMODULE hExe = nullptr, hCommon = nullptr;
    uint8_t* exe = MapImage(dir + L"\\witcher3.exe", hExe);
    uint8_t* common = MapImage(dir + L"\\PhysX3Common_x64.dll", hCommon);
    if (!exe || !common)
        return 2;
    wc::pe::Module m;
    if (!m.Init(exe))
        return 2;
    WLOG("witcher3.exe TimeDateStamp 0x%08X SizeOfImage 0x%zX", m.timeDateStamp, m.imageSize);

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    wc::collision::Offsets o;
    const bool ok = wc::collision::ResolveImage(m, o);
    QueryPerformanceCounter(&t1);
    WLOG("collision resolve: %s in %.1f ms", ok ? "OK" : "FAILED", (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    wc::collision::DumpOffsets(m, o);

    // Expected for 5.0.0.1044392 (0x6ABD8695).
    int mism = 0;
    if (m.timeDateStamp == 0x6ABD8695)
    {
        auto check = [&](const char* what, unsigned long long got, unsigned long long want) {
            const bool same = got == want;
            mism += same ? 0 : 1;
            WLOG("  %-34s 0x%llX %s (reference 0x%llX)", what, got, same ? "==" : "!= MISMATCH", want);
        };
        check("NpPhysics::mInstance RVA", m.Rva(o.npPhysicsInstance), 0x5426770);
        check("PxScene raycast slot", o.raycastSlot, 0x2A0);
        check("PxScene overlap slot", o.overlapSlot, 0x2B0);
        check("world scene offset", o.worldSceneOff, 0x60A60);
        check("world busy offset", o.worldBusyOff, 0x60A68);
        check("world query counter offset", o.worldQueryCountOff, 0x60A6C);
        check("group registry RVA", m.Rva(o.groupRegistry), 0x5A4EEC0);
    }

    const char* const exports[] = {
        "?findOverlapTriangleMesh@PxMeshQuery@physx@@SAIAEBVPxGeometry@2@AEBVPxTransform@2@AEBVPxTriangleMeshGeometry@2@1PEAIIIAEA_N@Z",
        "?findOverlapHeightField@PxMeshQuery@physx@@SAIAEBVPxGeometry@2@AEBVPxTransform@2@AEBVPxHeightFieldGeometry@2@1PEAIIIAEA_N@Z",
        "?getTriangle@PxMeshQuery@physx@@SAXAEBVPxTriangleMeshGeometry@2@AEBVPxTransform@2@IAEAVPxTriangle@2@PEAI3@Z",
        "?getTriangle@PxMeshQuery@physx@@SAXAEBVPxHeightFieldGeometry@2@AEBVPxTransform@2@IAEAVPxTriangle@2@PEAI3@Z",
        "?getNbPolygons@ConvexMesh@Gu@physx@@UEBAIXZ",
        "?getNbVertices@ConvexMesh@Gu@physx@@UEBAIXZ",
        "?getPolygonData@ConvexMesh@Gu@physx@@UEBA_NIAEAUPxHullPolygon@3@@Z",
        "?getVertices@ConvexMesh@Gu@physx@@UEBAPEBVPxVec3@3@XZ",
        "?getIndexBuffer@ConvexMesh@Gu@physx@@UEBAPEBEXZ",
    };
    int missing = 0;
    for (const char* e : exports)
        if (!HasExport(common, e))
        {
            ++missing;
            WLOG("  PhysX3Common export MISSING: %s", e);
        }
    WLOG("PhysX3Common_x64.dll: %d/%zu exports found", static_cast<int>(std::size(exports)) - missing, std::size(exports));
    WLOG("RESULT: %s (%d mismatch(es), %d missing export(s))", ok && mism == 0 && missing == 0 ? "PASS" : "FAIL", mism, missing);
    FreeLibrary(hCommon);
    FreeLibrary(hExe);
    return ok && mism == 0 && missing == 0 ? 0 : 1;
}

#ifndef WCRAFT_IN_FAKE_HOST
int wmain(int argc, wchar_t** argv)
{
    wc::log::InitConsole();
    return RunColScan(argc > 1 ? argv[1] : nullptr);
}
#endif
