// M5a native source: W3 5.0's real collision geometry (statically linked PhysX 3.3) as Minecraft
// triangles. See collision.hpp for the reverse engineering.
//
// How it reaches PhysX (all by signature / exported name, nothing at a fixed address):
//  - NpPhysics::mInstance (global) from PxCreateBasePhysics ("Wrong version: PhysX version is ...")
//    -> Ps::Array<NpScene*> at +0x08 / +0x10.
//  - Each scene's userData (+0x08) is a W3 object whose +0x18 points back to the owning
//    CPhysicsWorldPhysXImpl (validated: world->scene == scene). W3's own query wrapper (found by
//    its guard sequence) gives the world's scene / busy-flag / query-counter offsets and the
//    PxScene vtable slot of raycast; overlap is two slots later (raycast, sweep, overlap).
//  - We run PxScene::overlap with a box (static actors only, all hits as touches) and read each
//    shape through the public PxShape / PxRigidActor vtables (slots verified by their code shape
//    the first time a vtable is seen), then turn the geometry into world triangles. Meshes and
//    height fields go through PhysX3Common_x64.dll's exported PxMeshQuery (exact, clipped to the
//    box); convexes through Gu::ConvexMesh's exported getters; boxes/capsules/spheres by hand.
//  - Collision groups: W3's group registry (a CName list; bit i = name i), the same one
//    CWorld::StaticTrace uses. A shape counts if its query filter type mask hits CollisionGroups.
//  - Thread / safety: only from the game (script) thread, inside W3's own query guard
//    (lock inc counter; skip while "busy"; lock dec), every PhysX access under SEH; 3 faults or a
//    failed validation switch the source off. Kill switch: Collision=0 in the ini.
#include "collision.hpp"
#include "blocks.hpp"

#include "log.hpp"
#include "pattern.hpp"
#include "pe.hpp"
#include "red_engine.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace wc::collision
{
namespace
{
using pattern::FindAll;
using pattern::MatchAt;
using pattern::Parse;
using pattern::Rel;

// ---- PhysX 3.3 public ABI (x64) ------------------------------------------------------------
struct PxVec3
{
    float x, y, z;
};
struct PxQuat
{
    float x, y, z, w;
};
struct PxTransform
{
    PxQuat q;
    PxVec3 p;
};
struct PxBoxGeometry
{
    int32_t type; // 3
    PxVec3 halfExtents;
};
struct PxQueryFilterData
{
    uint32_t data[4];
    uint16_t flags;
    uint8_t clientId;
    uint8_t pad;
};
struct PxHullPolygon
{
    float plane[4];
    uint16_t nbVerts;
    uint16_t indexBase;
};
struct PxTriangle
{
    PxVec3 v[3];
};
// PxOverlapHit (PxQueryHit {actor, shape, faceIndex} padded to 24, + padTo16Bytes) = 32 bytes;
// W3's own overlap buffers step 32 bytes per touch.
struct OverlapHit
{
    void* actor;
    void* shape;
    uint32_t faceIndex;
    uint32_t pad0;
    uint32_t padTo16Bytes;
    uint32_t pad1;
};
static_assert(sizeof(OverlapHit) == 32);

constexpr int32_t kGeoSphere = 0, kGeoPlane = 1, kGeoCapsule = 2, kGeoBox = 3, kGeoConvex = 4, kGeoTriMesh = 5, kGeoHeightField = 6;
constexpr uint16_t kQueryStatic = 1u << 0, kQueryNoBlock = 1u << 5;

// PxShape vtable (byte offsets), PhysX 3.3 declaration order; verified per vtable at runtime.
constexpr size_t kShGetGeometryType = 0x28;
constexpr size_t kShGetBox = 0x40, kShGetSphere = 0x48, kShGetCapsule = 0x50, kShGetConvex = 0x60, kShGetTriMesh = 0x68,
                 kShGetHeightField = 0x70;
constexpr size_t kShGetLocalPose = 0x88, kShGetQueryFilterData = 0xB0;
// PxRigidActor vtable.
constexpr size_t kAcGetType = 0x28, kAcGetGlobalPose = 0xA0;

// PxHitCallback<PxOverlapHit>: vptr, block (32), hasBlock @0x28, touches @0x30, maxNbTouches @0x38,
// nbTouches @0x3C; virtuals: processTouches, finalizeQuery, ~dtor.
struct OverlapCallback
{
    virtual bool processTouches(const OverlapHit* buffer, uint32_t nbHits);
    virtual void finalizeQuery() {}
    virtual ~OverlapCallback() = default;

    OverlapHit block{};
    bool hasBlock = false;
    OverlapHit* touches = nullptr;
    uint32_t maxNbTouches = 0;
    uint32_t nbTouches = 0;
    // ours (after PhysX's fields)
    OverlapHit* out = nullptr;
    uint32_t outCap = 0, outCount = 0;
};
static_assert(offsetof(OverlapCallback, hasBlock) == 0x28);
static_assert(offsetof(OverlapCallback, touches) == 0x30);
static_assert(offsetof(OverlapCallback, nbTouches) == 0x3C);

bool OverlapCallback::processTouches(const OverlapHit* buffer, uint32_t nbHits)
{
    for (uint32_t i = 0; i < nbHits && outCount < outCap; ++i)
        out[outCount++] = buffer[i];
    return outCount < outCap; // keep going while we have room
}

using OverlapFn = bool (*)(void* scene, const void* geometry, const PxTransform* pose, OverlapCallback* cb, const PxQueryFilterData* fd,
                           void* filterCall);
using GetTypeFn = int32_t (*)(void* self);
using GetGeomFn = bool (*)(void* shape, void* out);
using GetPoseFn = PxTransform* (*)(void* self, PxTransform* ret);
using GetFilterFn = uint32_t* (*)(void* shape, uint32_t* ret);
using FindOverlapFn = uint32_t (*)(const void* geom, const PxTransform* pose, const void* meshGeom, const PxTransform* meshPose, uint32_t* results,
                                   uint32_t maxResults, uint32_t startIndex, bool* overflow);
using GetTriangleFn = void (*)(const void* meshGeom, const PxTransform* pose, uint32_t index, PxTriangle* tri, uint32_t* vertexIndices,
                               uint32_t* adjacency);
using CvxCountFn = uint32_t (*)(const void* mesh);
using CvxPolyFn = bool (*)(const void* mesh, uint32_t index, PxHullPolygon* out);
using CvxVertsFn = const PxVec3* (*)(const void* mesh);
using CvxIndexFn = const uint8_t* (*)(const void* mesh);

struct Exports
{
    FindOverlapFn findMesh = nullptr, findHf = nullptr;
    GetTriangleFn triMesh = nullptr, triHf = nullptr;
    CvxCountFn cvxNbPolys = nullptr, cvxNbVerts = nullptr;
    CvxPolyFn cvxPoly = nullptr;
    CvxVertsFn cvxVerts = nullptr;
    CvxIndexFn cvxIndex = nullptr;
};

const char* const kExportNames[] = {
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

// Export lookup on a mapped image (live module or LOAD_LIBRARY_AS_IMAGE_RESOURCE mapping).
void* FindExport(uint8_t* base, const char* name)
{
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return nullptr;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress)
        return nullptr;
    auto* ex = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    auto* names = reinterpret_cast<uint32_t*>(base + ex->AddressOfNames);
    auto* ords = reinterpret_cast<uint16_t*>(base + ex->AddressOfNameOrdinals);
    auto* funcs = reinterpret_cast<uint32_t*>(base + ex->AddressOfFunctions);
    for (uint32_t i = 0; i < ex->NumberOfNames; ++i)
        if (std::strcmp(reinterpret_cast<const char*>(base + names[i]), name) == 0)
            return base + funcs[ords[i]];
    return nullptr;
}

bool ResolveExports(uint8_t* base, Exports& e, int& missing)
{
    void* p[std::size(kExportNames)];
    missing = 0;
    for (size_t i = 0; i < std::size(kExportNames); ++i)
    {
        p[i] = FindExport(base, kExportNames[i]);
        if (!p[i])
        {
            ++missing;
            WLOG("collision: PhysX3Common export missing: %s", kExportNames[i]);
        }
    }
    e.findMesh = reinterpret_cast<FindOverlapFn>(p[0]);
    e.findHf = reinterpret_cast<FindOverlapFn>(p[1]);
    e.triMesh = reinterpret_cast<GetTriangleFn>(p[2]);
    e.triHf = reinterpret_cast<GetTriangleFn>(p[3]);
    e.cvxNbPolys = reinterpret_cast<CvxCountFn>(p[4]);
    e.cvxNbVerts = reinterpret_cast<CvxCountFn>(p[5]);
    e.cvxPoly = reinterpret_cast<CvxPolyFn>(p[6]);
    e.cvxVerts = reinterpret_cast<CvxVertsFn>(p[7]);
    e.cvxIndex = reinterpret_cast<CvxIndexFn>(p[8]);
    return missing == 0;
}

// ---- signatures (witcher3.exe 5.0) -----------------------------------------------------------
// W3's raycast wrapper (CPhysicsWorldPhysXImpl, vtable slot 1): its query guard and scene load.
//   lock inc dword [rbx+cnt] ; mov eax,[rbx+busy] ; test eax,eax ; jne ... ; vmovdqu xmm0,[rip+..] ; mov rcx,[rbx+scene]
// ... followed (within 0xE0 bytes) by `call [rax+raycastSlot]`.
constexpr const char* kGuardPat = "F0 FF 83 ?? ?? ?? ?? 8B 83 ?? ?? ?? ?? 85 C0 0F 85 ?? ?? ?? ?? C5 FA 6F 05 ?? ?? ?? ?? 48 8B 8B ?? ?? ?? ??";
// CWorld::StaticTrace native: the collision-group registry (CName list) behind the group names.
//   mov r11,[rip+registry] ; xor edx,edx ; mov r8d,[r11+10h] ; test r8d,r8d ; jz ; mov r9,[r11+8]
constexpr const char* kGroupsPat = "4C 8B 1D ?? ?? ?? ?? 33 D2 45 8B 43 10 45 85 C0 74 ?? 4D 8B 4B 08";
constexpr const char* kVersionString = "Wrong version: PhysX version is 0x%08x, tried to create 0x%08x";

uint32_t U32(const uint8_t* p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

// ---- settings / state ----------------------------------------------------------------------
struct Settings
{
    bool enabled = true;      // Collision
    int radius = 2;           // CollisionRadius (region columns around Minecraft's player)
    int budgetUs = 1000;      // CollisionBudgetUs per WCraft_ColScan call
    int refreshMs = 1500;     // CollisionRefreshMs (near columns; farther ones x2.5)
    int maxTris = 12000;      // CollisionMaxTris per column
    bool terrain = true;      // CollisionTerrain (PhysX height fields)
    int below = 12, above = 20; // CollisionBelow / CollisionAbove (blocks around the feet)
    int budgetFastUs = 2500;  // M6b CollisionBudgetFastUs: budget per call at flight speed (>= 12 blocks/s)
    int budgetMaxUs = 5000;   // M6b CollisionBudgetMaxUs: hard cap, only for columns needed within 0.5 s
    std::string groups = "Static,Terrain,Platforms,Fence,Destructible,Boat,Door,Foliage";
};

struct Stats
{
    uint64_t calls = 0, columns = 0, tris = 0, shapes = 0, busy = 0, faults = 0, truncated = 0, skippedGroup = 0, skippedType = 0;
    uint64_t skippedStandIn = 0; // WitcherCraft's block collision boxes left out
    uint64_t byType[8] = {};
    double callMsSum = 0, callMsMax = 0, colMsMax = 0;
    // M6b: calls while Minecraft's player moves fast (>= 8 blocks/s), and columns it needed soon
    uint64_t fastCalls = 0, aheadCols = 0, mustOver = 0, behind = 0;
    double fastMsSum = 0, fastMsMax = 0, speedMax = 0;
};

std::mutex g_mutex; // guards everything below (script thread + status readers)
Settings g_set;
Offsets g_off;
Exports g_ex;
bool g_inited = false, g_ok = false;
std::atomic<bool> g_enabled{true};
std::string g_why = "not initialised";
uint64_t g_includeMask = 0; // 0 = not resolved yet
bool g_masksTried = false;
std::vector<void*> g_goodShapeVt, g_goodActorVt, g_badVt;
std::unordered_map<uint64_t, uint64_t> g_maskSeen; // type mask -> shapes (diagnostics)
Stats g_stats;
std::atomic<StandInSource> g_standInSource{nullptr};
uint32_t g_lastScenes = 0, g_lastWorlds = 0;
uint64_t g_lastLogMs = 0;

std::wstring IniPath()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&IniPath), &self);
    wchar_t path[MAX_PATH * 2]{};
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2)
        return L"";
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash)
        return L"";
    wcscpy_s(slash + 1, MAX_PATH * 2 - (slash + 1 - path), L"witchercraft_native.ini");
    return path;
}

void LoadSettings()
{
    const std::wstring ini = IniPath();
    const wchar_t* sec = L"WitcherCraft";
    Settings s;
    s.enabled = GetPrivateProfileIntW(sec, L"Collision", 1, ini.c_str()) != 0;
    s.radius = std::clamp(static_cast<int>(GetPrivateProfileIntW(sec, L"CollisionRadius", 2, ini.c_str())), 1, 4);
    s.budgetUs = std::clamp(static_cast<int>(GetPrivateProfileIntW(sec, L"CollisionBudgetUs", 1000, ini.c_str())), 100, 20000);
    s.refreshMs = std::clamp(static_cast<int>(GetPrivateProfileIntW(sec, L"CollisionRefreshMs", 1500, ini.c_str())), 250, 60000);
    s.maxTris = std::clamp(static_cast<int>(GetPrivateProfileIntW(sec, L"CollisionMaxTris", 12000, ini.c_str())), 500, 100000);
    s.terrain = GetPrivateProfileIntW(sec, L"CollisionTerrain", 1, ini.c_str()) != 0;
    s.below = std::clamp(static_cast<int>(GetPrivateProfileIntW(sec, L"CollisionBelow", 12, ini.c_str())), 4, 64);
    s.above = std::clamp(static_cast<int>(GetPrivateProfileIntW(sec, L"CollisionAbove", 20, ini.c_str())), 4, 64);
    s.budgetFastUs = std::clamp(static_cast<int>(GetPrivateProfileIntW(sec, L"CollisionBudgetFastUs", 2500, ini.c_str())), s.budgetUs, 20000);
    s.budgetMaxUs = std::clamp(static_cast<int>(GetPrivateProfileIntW(sec, L"CollisionBudgetMaxUs", 5000, ini.c_str())), s.budgetFastUs, 30000);
    FlightConfig fc = GetFlightConfig();
    fc.lookaheadMs = std::clamp(static_cast<int>(GetPrivateProfileIntW(sec, L"CollisionLookaheadMs", 1500, ini.c_str())), 0, 5000);
    fc.guardMs = std::clamp(static_cast<int>(GetPrivateProfileIntW(sec, L"CollisionGuardMs", 150, ini.c_str())), 0, 2000);
    SetFlightConfig(fc);
    wchar_t groups[512]{};
    GetPrivateProfileStringW(sec, L"CollisionGroups", L"Static,Terrain,Platforms,Fence,Destructible,Boat,Door,Foliage", groups, 512, ini.c_str());
    s.groups.clear();
    for (const wchar_t* c = groups; *c; ++c)
        if (*c != L' ')
            s.groups += static_cast<char>(*c);
    g_set = s;
    WLOG("collision: ini Collision=%d CollisionRadius=%d CollisionBudgetUs=%d CollisionRefreshMs=%d CollisionMaxTris=%d CollisionTerrain=%d "
         "CollisionBelow=%d CollisionAbove=%d CollisionGroups=%s | flight: CollisionLookaheadMs=%d CollisionGuardMs=%d CollisionBudgetFastUs=%d "
         "CollisionBudgetMaxUs=%d",
         s.enabled, s.radius, s.budgetUs, s.refreshMs, s.maxTris, s.terrain, s.below, s.above, s.groups.c_str(), GetFlightConfig().lookaheadMs,
         GetFlightConfig().guardMs, s.budgetFastUs, s.budgetMaxUs);
}

bool ReadRegistry(const uint32_t*& ids, uint32_t& count)
{
    ids = nullptr;
    count = 0;
    __try
    {
        auto* reg = *reinterpret_cast<uint8_t**>(g_off.groupRegistry);
        if (reg)
        {
            ids = *reinterpret_cast<uint32_t**>(reg + 8);
            count = *reinterpret_cast<uint32_t*>(reg + 0x10);
        }
        return ids != nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

int FindBit(const uint32_t* ids, uint32_t count, uint32_t id)
{
    __try
    {
        for (uint32_t i = 0; i < count && i < 48; ++i)
            if (ids[i] == id)
                return static_cast<int>(i);
        return -1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

// Collision group names -> W3 type mask bits (the registry list index = bit).
void ResolveGroupMask()
{
    if (g_masksTried)
        return;
    g_masksTried = true;
    if (!g_off.groupRegistry || !red::g.AddName || !red::g.GetNamePool)
    {
        WLOG("collision: group registry or CName pool not resolved: every static shape with a type mask counts");
        return;
    }
    const uint32_t* ids = nullptr;
    uint32_t count = 0;
    if (!ReadRegistry(ids, count) || count == 0 || count > 64)
    {
        static int logged = 0;
        if (logged++ == 0)
            WLOG("collision: group registry empty/unreadable (count %u): every static shape with a type mask counts until it is", count);
        g_masksTried = false; // try again next call (the registry is filled at engine init)
        return;
    }
    uint64_t mask = 0;
    std::string names = g_set.groups + ",";
    std::string found, missing;
    size_t pos = 0;
    while (pos < names.size())
    {
        const size_t comma = names.find(',', pos);
        const std::string n = names.substr(pos, comma - pos);
        pos = comma + 1;
        if (n.empty())
            continue;
        const uint32_t id = red::g.AddName(red::g.GetNamePool(), n.c_str());
        const int bit = FindBit(ids, count, id);
        if (bit >= 0)
        {
            mask |= 1ull << bit;
            found += n + "=" + std::to_string(bit) + " ";
        }
        else
            missing += n + " ";
    }
    g_includeMask = mask;
    WLOG("collision: %u collision groups registered; include mask 0x%llX (%s) %s%s", count, static_cast<unsigned long long>(mask), found.c_str(),
         missing.empty() ? "" : "not found: ", missing.c_str());
}

// ---- math ------------------------------------------------------------------------------------
PxVec3 Rotate(const PxQuat& q, const PxVec3& v)
{
    // v + 2w(u x v) + 2 u x (u x v)
    const float ux = q.x, uy = q.y, uz = q.z;
    const float tx = 2.0f * (uy * v.z - uz * v.y), ty = 2.0f * (uz * v.x - ux * v.z), tz = 2.0f * (ux * v.y - uy * v.x);
    return {v.x + q.w * tx + (uy * tz - uz * ty), v.y + q.w * ty + (uz * tx - ux * tz), v.z + q.w * tz + (ux * ty - uy * tx)};
}
PxQuat Mul(const PxQuat& a, const PxQuat& b)
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y + a.y * b.w + a.z * b.x - a.x * b.z,
            a.w * b.z + a.z * b.w + a.x * b.y - a.y * b.x, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
PxTransform Compose(const PxTransform& a, const PxTransform& b)
{
    PxTransform r;
    r.q = Mul(a.q, b.q);
    const PxVec3 bp = Rotate(a.q, b.p);
    r.p = {a.p.x + bp.x, a.p.y + bp.y, a.p.z + bp.z};
    return r;
}
PxVec3 Apply(const PxTransform& t, const PxVec3& v)
{
    const PxVec3 r = Rotate(t.q, v);
    return {r.x + t.p.x, r.y + t.p.y, r.z + t.p.z};
}
bool FiniteTf(const PxTransform& t)
{
    const float* f = &t.q.x;
    for (int i = 0; i < 7; ++i)
        if (!std::isfinite(f[i]) || std::fabs(f[i]) > 1.0e6f)
            return false;
    const float n = t.q.x * t.q.x + t.q.y * t.q.y + t.q.z * t.q.z + t.q.w * t.q.w;
    return std::fabs(n - 1.0f) < 0.05f;
}

// ---- one column scan (SEH-only function: no C++ objects with destructors) -----------------
struct ColumnJob
{
    // in
    void* scene;
    OverlapFn overlap;
    float boxMin[3], boxMax[3]; // W3 coords
    float mcMin[3], mcMax[3];   // MC coords (for clipping)
    uint64_t includeMask;
    bool terrain;
    OverlapCallback* cb;
    OverlapHit* hits;
    uint32_t hitCap;
    uint32_t* idx;
    uint32_t idxCap;
    proto::ColTri* tris;
    uint32_t triCap;
    // out
    uint32_t triCount;
    uint32_t shapes;
    bool truncated;
    uint32_t badVtable; // 1 = shape vtable failed validation, 2 = actor vtable
    void* badVtPtr;
    uint64_t byType[8];
    uint32_t skippedGroup, skippedType;
    // WitcherCraft's block collision boxes (SetStandInSource): sorted cell keys, and the bounds of
    // everything the shape being extracted offered (before the column clip), MC coords.
    const uint64_t* standIns;
    size_t standInCount;
    float shapeLo[3], shapeHi[3];
    uint32_t skippedStandIn;
};

bool InList(const std::vector<void*>& v, void* p)
{
    return std::find(v.begin(), v.end(), p) != v.end();
}

// Code-shape checks of a PxShape vtable (NpShape): the 7 getXGeometry functions compare the
// geometry type with 3,0,2,1,4,5,6 in that order, getGeometryType reads the same field,
// getQueryFilterData copies 16 bytes out through the hidden return pointer.
bool CheckShapeVtable(void** vt)
{
    __try
    {
        const uint8_t* gt = static_cast<const uint8_t*>(vt[kShGetGeometryType / 8]);
        if (gt[0] != 0x8B || gt[1] != 0x81 || gt[6] != 0xC3)
            return false;
        const uint32_t typeOff = U32(gt + 2);
        const int expect[7] = {3, 0, 2, 1, 4, 5, 6};
        for (int i = 0; i < 7; ++i)
        {
            const uint8_t* f = static_cast<const uint8_t*>(vt[kShGetBox / 8 + i]);
            bool ok = false;
            for (int k = 0; k < 0x20 && !ok; ++k)
                ok = f[k] == 0x83 && f[k + 1] == 0xB9 && U32(f + k + 2) == typeOff && f[k + 6] == expect[i];
            if (!ok)
                return false;
        }
        const uint8_t* qf = static_cast<const uint8_t*>(vt[kShGetQueryFilterData / 8]);
        static const uint8_t kQf[] = {0x0F, 0x10, 0x41};
        static const uint8_t kQf2[] = {0x48, 0x8B, 0xC2, 0x0F, 0x11, 0x02, 0xC3};
        if (std::memcmp(qf, kQf, 3) != 0 || std::memcmp(qf + 4, kQf2, sizeof(kQf2)) != 0)
            return false;
        const uint8_t* lp = static_cast<const uint8_t*>(vt[kShGetLocalPose / 8]);
        bool movRdiRdx = false, test4 = false;
        for (int k = 0; k < 0x20; ++k)
        {
            movRdiRdx |= lp[k] == 0x48 && lp[k + 1] == 0x8B && lp[k + 2] == 0xFA;
            test4 |= lp[k] == 0xF6 && lp[k + 1] == 0xC2 && lp[k + 2] == 0x04;
        }
        return movRdiRdx && test4;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// NpRigidStatic: getType is `xor eax,eax; ret` (eRIGID_STATIC), getGlobalPose returns through rdx.
bool CheckStaticActorVtable(void** vt)
{
    __try
    {
        const uint8_t* gt = static_cast<const uint8_t*>(vt[kAcGetType / 8]);
        if (!(gt[0] == 0x33 && gt[1] == 0xC0 && gt[2] == 0xC3))
            return false;
        const uint8_t* gp = static_cast<const uint8_t*>(vt[kAcGetGlobalPose / 8]);
        for (int k = 0; k < 0x20; ++k)
            if (gp[k] == 0x48 && gp[k + 1] == 0x8B && gp[k + 2] == 0xFA)
                return true;
        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void EmitTri(ColumnJob& j, const PxVec3& a, const PxVec3& b, const PxVec3& c, uint32_t flags)
{
    // W3 -> MC: (x, z, -y)
    const float v[9] = {a.x, a.z, -a.y, b.x, b.z, -b.y, c.x, c.z, -c.y};
    for (int k = 0; k < 3; ++k)
    {
        j.shapeLo[k] = std::min({j.shapeLo[k], v[k], v[3 + k], v[6 + k]});
        j.shapeHi[k] = std::max({j.shapeHi[k], v[k], v[3 + k], v[6 + k]});
    }
    for (int k = 0; k < 3; ++k)
    {
        const float lo = std::min({v[k], v[3 + k], v[6 + k]}), hi = std::max({v[k], v[3 + k], v[6 + k]});
        if (hi < j.mcMin[k] - 0.01f || lo > j.mcMax[k] + 0.01f)
            return;
    }
    const float ux = v[3] - v[0], uy = v[4] - v[1], uz = v[5] - v[2], wx = v[6] - v[0], wy = v[7] - v[1], wz = v[8] - v[2];
    const float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
    if (nx * nx + ny * ny + nz * nz < 1e-10f)
        return; // degenerate
    if (j.triCount >= j.triCap)
    {
        j.truncated = true;
        return;
    }
    proto::ColTri& t = j.tris[j.triCount++];
    std::memcpy(t.v, v, sizeof(v));
    t.flags = flags;
}

void EmitBox(ColumnJob& j, const PxTransform& pose, float hx, float hy, float hz)
{
    PxVec3 c[8];
    for (int i = 0; i < 8; ++i)
        c[i] = Apply(pose, {(i & 1) ? hx : -hx, (i & 2) ? hy : -hy, (i & 4) ? hz : -hz});
    static const int f[6][4] = {{0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5}};
    for (const auto& q : f)
    {
        EmitTri(j, c[q[0]], c[q[1]], c[q[2]], 0);
        EmitTri(j, c[q[0]], c[q[2]], c[q[3]], 0);
    }
}

// One of WitcherCraft's block collision boxes: at most 1.2 m across, centred (within 0.1 m) in a cell
// that has one. The box template is exactly 1 m, centred on its pivot, which sits in the cell's centre.
bool IsStandIn(const ColumnJob& j)
{
    int cell[3];
    for (int k = 0; k < 3; ++k)
    {
        if (!(j.shapeHi[k] >= j.shapeLo[k]) || j.shapeHi[k] - j.shapeLo[k] > 1.2f)
            return false;
        const float c = (j.shapeLo[k] + j.shapeHi[k]) * 0.5f;
        cell[k] = static_cast<int>(std::floor(c));
        if (std::fabs(c - (cell[k] + 0.5f)) > 0.1f)
            return false;
    }
    return std::binary_search(j.standIns, j.standIns + j.standInCount, blocks::CellKey(cell[0], cell[1], cell[2]));
}

void ExtractGeometry(ColumnJob& j, const Exports& ex, void* shape, void** svt, int32_t type, const PxTransform& pose, uint8_t* geom,
                     const PxBoxGeometry& qbox, const PxTransform& qpose);

void ExtractShape(ColumnJob& j, const Exports& ex, void* actor, void* shape, const PxBoxGeometry& qbox, const PxTransform& qpose)
{
    void** svt = *static_cast<void***>(shape);
    if (!InList(g_goodShapeVt, svt))
    {
        if (InList(g_badVt, svt) || !CheckShapeVtable(svt))
        {
            j.badVtable = 1;
            j.badVtPtr = svt;
            return;
        }
        g_goodShapeVt.push_back(svt);
    }
    void** avt = *static_cast<void***>(actor);
    if (!InList(g_goodActorVt, avt))
    {
        if (!CheckStaticActorVtable(avt))
        {
            ++j.skippedType; // not a rigid static (shouldn't happen with eSTATIC): skip quietly
            return;
        }
        g_goodActorVt.push_back(avt);
    }
    const int32_t type = reinterpret_cast<GetTypeFn>(svt[kShGetGeometryType / 8])(shape);
    if (type < 0 || type > 6)
        return;
    alignas(16) uint32_t fd[4] = {};
    reinterpret_cast<GetFilterFn>(svt[kShGetQueryFilterData / 8])(shape, fd);
    const uint64_t typeMask = (static_cast<uint64_t>(fd[0]) | (static_cast<uint64_t>(fd[1]) << 32)) & 0xFFFFFFFFFFFFull;
    {
        auto& n = g_maskSeen[typeMask];
        ++n;
    }
    if (typeMask == 0 || (j.includeMask && !(typeMask & j.includeMask)))
    {
        ++j.skippedGroup;
        return;
    }
    if (type == kGeoHeightField && !j.terrain)
    {
        ++j.skippedType;
        return;
    }
    PxTransform local{}, global{};
    reinterpret_cast<GetPoseFn>(svt[kShGetLocalPose / 8])(shape, &local);
    reinterpret_cast<GetPoseFn>(avt[kAcGetGlobalPose / 8])(actor, &global);
    if (!FiniteTf(local) || !FiniteTf(global))
        return;
    const PxTransform pose = Compose(global, local);
    alignas(16) uint8_t geom[0x40] = {};
    ++j.shapes;
    ++j.byType[type];
    const uint32_t firstTri = j.triCount;
    for (int k = 0; k < 3; ++k)
    {
        j.shapeLo[k] = INFINITY;
        j.shapeHi[k] = -INFINITY;
    }
    ExtractGeometry(j, ex, shape, svt, type, pose, geom, qbox, qpose);
    if (j.standInCount && j.triCount > firstTri && IsStandIn(j))
    {
        j.triCount = firstTri; // Minecraft's block is Steve's collision here, not the box
        ++j.skippedStandIn;
    }
}

// The shape's geometry as triangles (EmitTri / EmitBox).
void ExtractGeometry(ColumnJob& j, const Exports& ex, void* shape, void** svt, int32_t type, const PxTransform& pose, uint8_t* geom,
                     const PxBoxGeometry& qbox, const PxTransform& qpose)
{
    switch (type)
    {
    case kGeoBox:
    {
        if (!reinterpret_cast<GetGeomFn>(svt[kShGetBox / 8])(shape, geom))
            return;
        PxVec3 he;
        std::memcpy(&he, geom + 4, sizeof(he));
        EmitBox(j, pose, he.x, he.y, he.z);
        break;
    }
    case kGeoSphere:
    {
        if (!reinterpret_cast<GetGeomFn>(svt[kShGetSphere / 8])(shape, geom))
            return;
        float r;
        std::memcpy(&r, geom + 4, 4);
        EmitBox(j, pose, r, r, r);
        break;
    }
    case kGeoCapsule:
    {
        if (!reinterpret_cast<GetGeomFn>(svt[kShGetCapsule / 8])(shape, geom))
            return;
        float r, hh;
        std::memcpy(&r, geom + 4, 4);
        std::memcpy(&hh, geom + 8, 4);
        EmitBox(j, pose, hh + r, r, r); // capsule axis = local X; a box is close enough for blocks
        break;
    }
    case kGeoConvex:
    {
        if (!reinterpret_cast<GetGeomFn>(svt[kShGetConvex / 8])(shape, geom))
            return;
        PxVec3 s;
        PxQuat sr;
        std::memcpy(&s, geom + 4, 12);
        std::memcpy(&sr, geom + 16, 16);
        const void* mesh = *reinterpret_cast<void**>(geom + 0x20);
        if (!mesh)
            return;
        const PxVec3* verts = ex.cvxVerts(mesh);
        const uint8_t* index = ex.cvxIndex(mesh);
        const uint32_t nv = ex.cvxNbVerts(mesh), np = ex.cvxNbPolys(mesh);
        const PxQuat inv{-sr.x, -sr.y, -sr.z, sr.w};
        auto toWorld = [&](uint8_t vi) {
            PxVec3 v = vi < nv ? verts[vi] : PxVec3{0, 0, 0};
            v = Rotate(sr, v); // PxMeshScale: R^T * S * R, with R = rotation (as a matrix of q^-1)
            v = {v.x * s.x, v.y * s.y, v.z * s.z};
            v = Rotate(inv, v);
            return Apply(pose, v);
        };
        for (uint32_t p = 0; p < np; ++p)
        {
            PxHullPolygon poly{};
            if (!ex.cvxPoly(mesh, p, &poly) || poly.nbVerts < 3)
                continue;
            const PxVec3 a = toWorld(index[poly.indexBase]);
            for (uint32_t k = 1; k + 1 < poly.nbVerts; ++k)
                EmitTri(j, a, toWorld(index[poly.indexBase + k]), toWorld(index[poly.indexBase + k + 1]), 0);
        }
        break;
    }
    case kGeoTriMesh:
    case kGeoHeightField:
    {
        const bool hf = type == kGeoHeightField;
        if (!reinterpret_cast<GetGeomFn>(svt[(hf ? kShGetHeightField : kShGetTriMesh) / 8])(shape, geom))
            return;
        const FindOverlapFn find = hf ? ex.findHf : ex.findMesh;
        const GetTriangleFn get = hf ? ex.triHf : ex.triMesh;
        uint32_t start = 0;
        for (int guard = 0; guard < 64; ++guard)
        {
            bool overflow = false;
            const uint32_t n = find(&qbox, &qpose, geom, &pose, j.idx, j.idxCap, start, &overflow);
            for (uint32_t i = 0; i < n && i < j.idxCap; ++i)
            {
                PxTriangle t{};
                get(geom, &pose, j.idx[i], &t, nullptr, nullptr);
                EmitTri(j, t.v[0], t.v[1], t.v[2], hf ? proto::kTriTerrain : 0u);
            }
            if (!overflow || j.truncated || n == 0)
                break;
            start += n;
        }
        break;
    }
    default:
        break; // planes: none in W3's world collision
    }
}

// Returns 0 = ok, 1 = PhysX overlap failed, 2 = access violation / exception.
int RunColumnSeh(ColumnJob& j, const Exports& ex)
{
    __try
    {
        PxBoxGeometry box{kGeoBox, {(j.boxMax[0] - j.boxMin[0]) * 0.5f, (j.boxMax[1] - j.boxMin[1]) * 0.5f, (j.boxMax[2] - j.boxMin[2]) * 0.5f}};
        PxTransform pose{{0, 0, 0, 1}, {(j.boxMax[0] + j.boxMin[0]) * 0.5f, (j.boxMax[1] + j.boxMin[1]) * 0.5f, (j.boxMax[2] + j.boxMin[2]) * 0.5f}};
        PxQueryFilterData fd{};
        fd.flags = kQueryStatic | kQueryNoBlock; // static actors, every hit a touch, no filter
        OverlapCallback* cb = j.cb;
        cb->hasBlock = false;
        cb->touches = j.hits;
        cb->maxNbTouches = j.hitCap / 2;
        cb->nbTouches = 0;
        cb->out = j.hits + j.hitCap / 2;
        cb->outCap = j.hitCap - j.hitCap / 2;
        cb->outCount = 0;
        j.overlap(j.scene, &box, &pose, cb, &fd, nullptr);
        // Touches PhysX handed to processTouches (buffer overflows) plus what's left in the buffer.
        uint32_t n = cb->outCount;
        OverlapHit* all = cb->out;
        for (uint32_t i = 0; i < cb->nbTouches && i < cb->maxNbTouches && n < cb->outCap; ++i)
            all[n++] = j.hits[i];
        if (cb->hasBlock && n < cb->outCap)
            all[n++] = cb->block;
        // Dedupe (shape, actor).
        std::sort(all, all + n, [](const OverlapHit& a, const OverlapHit& b) { return a.shape < b.shape || (a.shape == b.shape && a.actor < b.actor); });
        for (uint32_t i = 0; i < n; ++i)
        {
            if (i && all[i].shape == all[i - 1].shape && all[i].actor == all[i - 1].actor)
                continue;
            if (!all[i].shape || !all[i].actor)
                continue;
            ExtractShape(j, ex, all[i].actor, all[i].shape, box, pose);
            if (j.badVtable || j.truncated)
                break;
        }
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 2;
    }
}

// Reads the scenes and their owning W3 physics worlds (SEH). Returns how many usable.
struct SceneRef
{
    void* scene;
    uint8_t* world;
};
uint32_t FindScenes(SceneRef* out, uint32_t cap, uint32_t& total)
{
    uint32_t n = 0;
    total = 0;
    __try
    {
        auto* np = *reinterpret_cast<uint8_t**>(g_off.npPhysicsInstance);
        if (!np)
            return 0;
        void** scenes = *reinterpret_cast<void***>(np + g_off.scenesArrayOff);
        const uint32_t count = *reinterpret_cast<uint32_t*>(np + g_off.scenesCountOff);
        if (!scenes || count > 32)
            return 0;
        total = count;
        for (uint32_t i = 0; i < count && n < cap; ++i)
        {
            void* sc = scenes[i];
            if (!sc)
                continue;
            auto* ud = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(sc) + 8); // PxScene::userData
            if (!ud)
                continue;
            uint8_t* world = nullptr;
            for (uint32_t off = 0x18; off < 0x60 && !world; off += 8) // +0x18 in 5.0; scan a little for safety
            {
                auto* w = *reinterpret_cast<uint8_t**>(ud + off);
                if (w && reinterpret_cast<uintptr_t>(w) > 0x10000 && !(reinterpret_cast<uintptr_t>(w) & 7) &&
                    *reinterpret_cast<void**>(w + g_off.worldSceneOff) == sc)
                    world = w;
            }
            if (world)
                out[n++] = {sc, world};
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return n;
    }
    return n;
}

void Disable(const std::string& why)
{
    g_ok = false;
    g_why = why;
    SetSourceActive(false);
    WLOG("collision: native W3 collision OFF: %s", why.c_str());
}

int FloorDiv(int v, int d)
{
    return v >= 0 ? v / d : -((-v + d - 1) / d);
}

std::string NativeStatusLocked()
{
    char buf[768];
    const Stats& st = g_stats;
    std::snprintf(buf, sizeof(buf),
                  "native %s%s | scenes %u | %llu calls avg %.2f max %.2f ms | %llu columns (max %.2f ms) %llu tris %llu shapes "
                  "(mesh %llu hf %llu convex %llu box %llu capsule %llu sphere %llu) | skipped: group %llu other %llu block boxes %llu | busy %llu faults %llu "
                  "truncated %llu | groups 0x%llX | flight: %llu fast calls avg %.2f max %.2f ms, %llu columns ahead, %llu over budget "
                  "(needed < 0.5 s), %llu calls left due columns, top speed %.0f b/s",
                  g_ok ? "ON" : "OFF: ", g_ok ? (g_enabled.load() ? "" : " (disabled by console)") : g_why.c_str(), g_lastScenes,
                  static_cast<unsigned long long>(st.calls), st.calls ? st.callMsSum / st.calls : 0.0, st.callMsMax,
                  static_cast<unsigned long long>(st.columns), st.colMsMax, static_cast<unsigned long long>(st.tris),
                  static_cast<unsigned long long>(st.shapes), static_cast<unsigned long long>(st.byType[kGeoTriMesh]),
                  static_cast<unsigned long long>(st.byType[kGeoHeightField]), static_cast<unsigned long long>(st.byType[kGeoConvex]),
                  static_cast<unsigned long long>(st.byType[kGeoBox]), static_cast<unsigned long long>(st.byType[kGeoCapsule]),
                  static_cast<unsigned long long>(st.byType[kGeoSphere]), static_cast<unsigned long long>(st.skippedGroup),
                  static_cast<unsigned long long>(st.skippedType), static_cast<unsigned long long>(st.skippedStandIn),
                  static_cast<unsigned long long>(st.busy),
                  static_cast<unsigned long long>(st.faults), static_cast<unsigned long long>(st.truncated),
                  static_cast<unsigned long long>(g_includeMask), static_cast<unsigned long long>(st.fastCalls),
                  st.fastCalls ? st.fastMsSum / st.fastCalls : 0.0, st.fastMsMax, static_cast<unsigned long long>(st.aheadCols),
                  static_cast<unsigned long long>(st.mustOver), static_cast<unsigned long long>(st.behind), st.speedMax);
    return buf;
}
} // namespace

// ---- resolver ------------------------------------------------------------------------------
bool ResolveImage(const pe::Module& m, Offsets& o)
{
    o = Offsets{};
    const pe::Section* tx = m.Find(".text");
    const pe::Section* rd = m.Find(".rdata");
    if (!tx || !rd)
        return false;
    // 1) NpPhysics::mInstance: lea rdx,"Wrong version..." in PxCreateBasePhysics, then the first
    //    `mov [rip+X], rbx` after it is `NpPhysics::mInstance = new NpPhysics(...)`.
    const size_t sl = std::strlen(kVersionString) + 1;
    auto strs = pattern::FindBytes(rd->begin, rd->size, kVersionString, sl, 4);
    if (strs.size() == 1)
    {
        uint8_t* str = strs[0];
        const auto leaPat = Parse("48 8D 15 ?? ?? ?? ??");
        for (uint8_t* p : FindAll(tx->begin, tx->size, leaPat))
        {
            if (Rel(p, 3, 7) != str)
                continue;
            for (uint8_t* q = p + 7; q < p + 0x300 && m.In(q, 8); ++q)
                if (q[0] == 0x48 && q[1] == 0x89 && q[2] == 0x1D)
                {
                    uint8_t* g = Rel(q, 3, 7);
                    if (m.InSection(".data", g, 8))
                        o.npPhysicsInstance = g;
                    break;
                }
            break;
        }
    }
    // 2) W3's raycast wrapper: guard offsets, scene offset, raycast slot.
    auto guard = FindAll(tx->begin, tx->size, Parse(kGuardPat), 4);
    if (guard.size() == 1)
    {
        uint8_t* g = guard[0];
        o.worldQueryCountOff = U32(g + 3);
        o.worldBusyOff = U32(g + 9);
        o.worldSceneOff = U32(g + 32);
        for (uint8_t* q = g + 36; q < g + 0x120; ++q)
            if (q[0] == 0xFF && q[1] == 0x90 && q[4] == 0 && q[5] == 0)
            {
                o.raycastSlot = U32(q + 2);
                o.overlapSlot = o.raycastSlot + 0x10; // PxScene: raycast, sweep, overlap
                break;
            }
    }
    // 3) Collision-group registry (StaticTrace's CName -> bit lookup). Every hit must agree.
    for (uint8_t* p : FindAll(tx->begin, tx->size, Parse(kGroupsPat), 8))
    {
        uint8_t* reg = Rel(p, 3, 7);
        if (!m.InSection(".data", reg, 8))
            continue;
        if (o.groupRegistry && o.groupRegistry != reg)
        {
            o.groupRegistry = nullptr;
            break;
        }
        o.groupRegistry = reg;
        o.staticTraceNative = p;
    }
    const bool ok = o.npPhysicsInstance && o.overlapSlot && o.worldSceneOff && o.worldBusyOff && o.worldQueryCountOff &&
                    o.worldQueryCountOff + 4 <= 0x100000 && o.worldSceneOff < 0x100000;
    return ok;
}

void DumpOffsets(const pe::Module& m, const Offsets& o)
{
    WLOG("collision: NpPhysics::mInstance   RVA 0x%llX", static_cast<unsigned long long>(m.Rva(o.npPhysicsInstance)));
    WLOG("collision: PxScene raycast slot   +0x%X, overlap slot +0x%X", o.raycastSlot, o.overlapSlot);
    WLOG("collision: CPhysicsWorldPhysXImpl scene +0x%X, busy +0x%X, query counter +0x%X", o.worldSceneOff, o.worldBusyOff, o.worldQueryCountOff);
    WLOG("collision: collision-group registry RVA 0x%llX (StaticTrace site RVA 0x%llX)", static_cast<unsigned long long>(m.Rva(o.groupRegistry)),
         static_cast<unsigned long long>(m.Rva(o.staticTraceNative)));
}

bool InitNative()
{
    std::lock_guard lk(g_mutex);
    if (g_inited)
        return g_ok;
    g_inited = true;
    LoadSettings();
    g_enabled.store(g_set.enabled);
    if (!g_set.enabled)
    {
        Disable("Collision=0 in the ini");
        return false;
    }
    pe::Module m;
    if (!m.Init(GetModuleHandleW(nullptr)) || !ResolveImage(m, g_off))
    {
        DumpOffsets(m, g_off);
        Disable("PhysX signatures not found (different game build?)");
        return false;
    }
    DumpOffsets(m, g_off);
    HMODULE common = GetModuleHandleW(L"PhysX3Common_x64.dll");
    int missing = 0;
    if (!common || !ResolveExports(reinterpret_cast<uint8_t*>(common), g_ex, missing))
    {
        Disable(common ? "PhysX3Common exports missing" : "PhysX3Common_x64.dll not loaded");
        return false;
    }
    g_ok = true;
    g_why = "ok";
    WLOG("collision: native W3 collision source ready (PhysX 3.3 overlap at vtable +0x%X)", g_off.overlapSlot);
    return true;
}

void SetStandInSource(StandInSource fn)
{
    g_standInSource.store(fn);
}

void SetEnabled(bool on)
{
    std::lock_guard lk(g_mutex);
    g_enabled.store(on);
    if (!on)
        SetSourceActive(false);
    WLOG("collision: native W3 collision %s by console", on ? "enabled" : "disabled");
}

int Scan(float w3x, float w3y, float w3z)
{
    if (!g_inited)
        InitNative();
    std::lock_guard lk(g_mutex);
    if (!g_ok || !g_enabled.load())
    {
        SetSourceActive(false);
        return -1;
    }
    ResolveGroupMask();

    LARGE_INTEGER f, t0;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    auto elapsedUs = [&] {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return static_cast<double>(t.QuadPart - t0.QuadPart) * 1e6 / static_cast<double>(f.QuadPart);
    };

    SceneRef scenes[8];
    uint32_t total = 0;
    const uint32_t ns = FindScenes(scenes, 8, total);
    if (ns != g_lastScenes || total != g_lastWorlds)
    {
        WLOG("collision: %u PhysX scene(s), %u with a W3 physics world", total, ns);
        g_lastScenes = ns;
        g_lastWorlds = total;
    }
    if (ns == 0)
    {
        SetSourceActive(false); // nothing to scan (loading): the hold gate mustn't wait for us
        return 0;
    }
    SetSourceActive(true);

    // Minecraft coordinates of the centre (feet, or a held teleport's target).
    const double mx = w3x, my = w3z, mz = -static_cast<double>(w3y);
    const int prx = FloorDiv(static_cast<int>(std::floor(mx)), 8), prz = FloorDiv(static_cast<int>(std::floor(mz)), 8);
    const uint64_t now = GetTickCount64();

    // M6b: Minecraft's motion (host thread, McState ticks). Used only when the centre is the
    // player itself (not a teleport target): prefetch along the path, adaptive budget.
    Motion mo;
    if (GetMotion(mo, now) && std::fabs(mo.x - mx) + std::fabs(mo.z - mz) > 6.0)
        mo.valid = false;
    if (!mo.valid)
        mo.x = mx, mo.y = my, mo.z = mz, mo.vx = mo.vy = mo.vz = 0.0;
    const double speed = mo.valid ? mo.Speed() : 0.0;
    // An ender pearl in flight: scan its path too, at the fast budget (Minecraft holds the pearl
    // until that collision is in).
    Motion focus;
    const bool haveFocus = GetFocus(focus, now);
    const bool fast = speed >= 8.0;
    const double ramp = haveFocus ? 1.0 : std::clamp((speed - 4.0) / 8.0, 0.0, 1.0);
    const double budgetUs = g_set.budgetUs + (g_set.budgetFastUs - g_set.budgetUs) * ramp;
    PlanSettings ps;
    ps.radius = g_set.radius;
    ps.refreshMs = g_set.refreshMs;
    ps.below = g_set.below;
    ps.above = g_set.above;
    static std::vector<PlanItem> cands;
    PlanScan(mx, my, mz, mo, ps, now, cands, haveFocus ? &focus : nullptr);
    static double colCostUs = 300.0; // running estimate of one column's cost
    const int R = g_set.radius;
    static std::vector<OverlapHit> hits(8192);
    static std::vector<uint32_t> idx(16384);
    static std::vector<proto::ColTri> tris;
    tris.resize(static_cast<size_t>(g_set.maxTris));
    OverlapCallback cb;
    static std::vector<uint64_t> standIns;
    standIns.clear();
    if (const StandInSource src = g_standInSource.load())
        src(standIns);
    int done = 0;
    bool left = false;
    for (const PlanItem& c : cands)
    {
        const double el = elapsedUs();
        // Columns the player reaches within 0.5 s that were never scanned (or not at this height)
        // may use the hard cap: a short W3 frame spike beats Steve falling through the world.
        const bool must = c.must;
        const double limit = must ? g_set.budgetMaxUs : budgetUs;
        if (done > 0 && el + colCostUs * 0.75 > limit)
        {
            left = true;
            break;
        }
        if (must && el + colCostUs * 0.75 > budgetUs)
            ++g_stats.mustOver;
        const int ryLo = c.ryLo, ryHi = c.ryHi;
        const double c0 = el;
        ColumnJob j{};
        j.overlap = nullptr;
        j.mcMin[0] = static_cast<float>(c.rx * 8);
        j.mcMax[0] = static_cast<float>(c.rx * 8 + 8);
        j.mcMin[1] = static_cast<float>(ryLo * 8);
        j.mcMax[1] = static_cast<float>(ryHi * 8 + 8);
        j.mcMin[2] = static_cast<float>(c.rz * 8);
        j.mcMax[2] = static_cast<float>(c.rz * 8 + 8);
        // MC -> W3 box: x = x, y = -z, z = y (slightly inflated so edge geometry is caught)
        j.boxMin[0] = j.mcMin[0] - 0.05f, j.boxMax[0] = j.mcMax[0] + 0.05f;
        j.boxMin[1] = -j.mcMax[2] - 0.05f, j.boxMax[1] = -j.mcMin[2] + 0.05f;
        j.boxMin[2] = j.mcMin[1] - 0.05f, j.boxMax[2] = j.mcMax[1] + 0.05f;
        j.includeMask = g_includeMask;
        j.terrain = g_set.terrain;
        j.cb = &cb;
        j.hits = hits.data();
        j.hitCap = static_cast<uint32_t>(hits.size());
        j.idx = idx.data();
        j.idxCap = static_cast<uint32_t>(idx.size());
        j.tris = tris.data();
        j.triCap = static_cast<uint32_t>(tris.size());
        j.standIns = standIns.data();
        j.standInCount = standIns.size();
        bool busy = false, fault = false;
        for (uint32_t s = 0; s < ns && !fault; ++s)
        {
            uint8_t* w = scenes[s].world;
            auto* counter = reinterpret_cast<volatile LONG*>(w + g_off.worldQueryCountOff);
            auto* busyFlag = reinterpret_cast<volatile LONG*>(w + g_off.worldBusyOff);
            j.scene = scenes[s].scene;
            void** svt = *static_cast<void***>(j.scene);
            j.overlap = reinterpret_cast<OverlapFn>(svt[g_off.overlapSlot / 8]);
            // W3's own query guard (as its raycast wrapper): announce the query, back off if busy.
            InterlockedIncrement(counter);
            if (*busyFlag != 0)
            {
                InterlockedDecrement(counter);
                busy = true;
                break;
            }
            const int r = RunColumnSeh(j, g_ex);
            InterlockedDecrement(counter);
            if (r != 0)
                fault = true;
        }
        if (busy)
        {
            ++g_stats.busy;
            break; // try again next call
        }
        if (fault)
        {
            ++g_stats.faults;
            WLOG("collision: exception while reading W3 collision (column %d,%d); fault %llu/3", c.rx, c.rz,
                 static_cast<unsigned long long>(g_stats.faults));
            if (g_stats.faults >= 3)
                Disable("3 exceptions while reading PhysX data");
            break;
        }
        if (j.badVtable)
        {
            g_badVt.push_back(j.badVtPtr);
            char why[160];
            std::snprintf(why, sizeof(why), "unexpected PxShape vtable layout at %p (PhysX build differs)", j.badVtPtr);
            Disable(why);
            break;
        }
        SetColumn(c.rx, c.rz, ryLo, ryHi, tris.data(), j.triCount, now);
        MarkScanned(c.rx, c.rz, ryLo, ryHi, j.byType[kGeoHeightField] > 0, now);
        ++done;
        if (std::max(std::abs(c.rx - prx), std::abs(c.rz - prz)) > R)
            ++g_stats.aheadCols;
        ++g_stats.columns;
        g_stats.tris += j.triCount;
        g_stats.shapes += j.shapes;
        g_stats.skippedGroup += j.skippedGroup;
        g_stats.skippedType += j.skippedType;
        g_stats.skippedStandIn += j.skippedStandIn;
        for (int k = 0; k < 8; ++k)
            g_stats.byType[k] += j.byType[k];
        if (j.truncated)
        {
            ++g_stats.truncated;
            if (g_stats.truncated <= 5)
                WLOG("collision: column %d,%d has more than %d triangles; truncated (CollisionMaxTris)", c.rx, c.rz, g_set.maxTris);
        }
        const double colUs = elapsedUs() - c0;
        colCostUs = colCostUs * 0.8 + colUs * 0.2;
        g_stats.colMsMax = std::max(g_stats.colMsMax, colUs / 1000.0);
        if (g_stats.columns <= 6)
            WLOG("collision: column %d,%d (ry %d..%d): %u shapes, %u triangles, %u skipped by group, %.2f ms", c.rx, c.rz, ryLo, ryHi, j.shapes,
                 j.triCount, j.skippedGroup, (elapsedUs() - c0) / 1000.0);
    }
    const double ms = elapsedUs() / 1000.0;
    ++g_stats.calls;
    g_stats.callMsSum += ms;
    g_stats.callMsMax = std::max(g_stats.callMsMax, ms);
    g_stats.speedMax = std::max(g_stats.speedMax, speed);
    if (left)
        ++g_stats.behind;
    if (fast)
    {
        ++g_stats.fastCalls;
        g_stats.fastMsSum += ms;
        g_stats.fastMsMax = std::max(g_stats.fastMsMax, ms);
    }
    if (now - g_lastLogMs > 30000)
    {
        g_lastLogMs = now;
        std::string masks;
        int shown = 0;
        for (auto& kv : g_maskSeen)
            if (shown++ < 12)
            {
                char b[48];
                std::snprintf(b, sizeof(b), "%llX:%llu ", static_cast<unsigned long long>(kv.first), static_cast<unsigned long long>(kv.second));
                masks += b;
            }
        WLOG("collision: %s | type masks seen (mask:shapes) %s", NativeStatusLocked().c_str(), masks.c_str());
    }
    return done;
}

std::string NativeStatus()
{
    std::lock_guard lk(g_mutex);
    return NativeStatusLocked() + " | " + StoreStatus();
}
} // namespace wc::collision
