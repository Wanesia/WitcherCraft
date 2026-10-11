#pragma once
// M5a: solid W3 walls and buildings for Minecraft.
//
// Two halves:
//  - the OBSTACLE STORE (collision.cpp, also linked into the offline tools): per 8x8 region column,
//    the triangles of W3's real collision geometry (MC coordinates), binned into 8x8x8 regions.
//    skycraft_host.cpp merges them with the script's terrain height field when it streams a
//    column (kColTris + kColRegion), so MC's smooth player collider and its voxels see walls,
//    floors, roofs, rocks, trees...
//  - the NATIVE PHYSX SOURCE (collision_physx.cpp, DLL + col_scan tool only): resolves W3 5.0's
//    statically linked PhysX 3.3 by signature, and on the game thread (WCraft_ColScan, from the
//    script's 30 Hz step) runs PxScene::overlap around Minecraft's player and extracts static
//    triangle meshes, height fields, convexes, boxes, capsules and spheres as world triangles.
#include "skycraft_protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace wc::pe
{
struct Module;
}

namespace wc::collision
{
namespace proto = skycraft::proto;

// ---- store (thread-safe) -----------------------------------------------------------------
// Replaces column (rx, rz)'s obstacle data for the vertical regions [ryLo, ryHi] (MC region
// coordinates, 8 blocks each). `tris` are in MC coordinates; each one is binned into every region
// of the column its bounding box touches. Bumps the column version (and queues it for the host)
// only if the data changed. Always records the scan time (for the hold gate).
void SetColumn(int rx, int rz, int ryLo, int ryHi, const proto::ColTri* tris, size_t count, uint64_t nowMs);
// Drops everything (world change).
void Clear();
// Drops columns further than `keep` region columns from (cx, cz) (MC block coords), except the
// ones on the predicted flight path (M6b, see Motion).
void Prune(double cx, double cz, int keep);

// Host side.
struct ColumnInfo
{
    uint64_t version = 0;  // bumps when the data changes
    uint64_t scanMs = 0;   // last time a scan of this column completed (data changed or not)
    int ryLo = 0, ryHi = -1;
    size_t tris = 0;
    size_t terrainTris = 0; // M6b: W3 PhysX height-field triangles (exact terrain) in this column
};
bool GetColumn(int rx, int rz, ColumnInfo& out);
// Appends the obstacle triangles stored for region (rx, ry, rz).
void AppendRegion(int rx, int ry, int rz, std::vector<proto::ColTri>& out);
// Columns whose data changed since the last call (packed as the host's Key(rx, rz)).
void TakeChanged(std::vector<std::pair<int, int>>& out);
// True while a source (the native scanner or a test) feeds the store: the hold gate then also
// waits for obstacle scans around a teleport target.
bool Active();
void SetSourceActive(bool on);
// Store counters for status lines.
std::string StoreStatus();

// ---- M6b: solid world at flight speed ----------------------------
// Minecraft's player motion, MC coords, blocks/s. The host thread sets it from McState's raw ticks
// (prev/cur feet); the scanner, the host's streaming and the flight guard read it.
struct Motion
{
    bool valid = false;
    double x = 0, y = 0, z = 0;    // feet
    double vx = 0, vy = 0, vz = 0; // blocks per second
    uint64_t ms = 0;               // when it was set
    double Speed() const;
    double HSpeed() const;
};
void SetMotion(const Motion& m);
// False if never set, invalid, or older than 500 ms (then `out` has valid = false).
bool GetMotion(Motion& out, uint64_t nowMs);
// A second scan focus: the player's ender pearl in flight (Minecraft holds it until the collision
// along its path is in). Same rules as GetMotion.
void SetFocus(const Motion& m);
bool GetFocus(Motion& out, uint64_t nowMs);

// Flight settings (ini, read by the native source; offline tools set them directly).
struct FlightConfig
{
    int lookaheadMs = 1500;    // CollisionLookaheadMs: prefetch this far along the velocity (0 = M5a behaviour)
    int maxAhead = 112;        // blocks: cap of the lookahead distance
    int guardMs = 150;         // CollisionGuardMs: path that must be ready, else the flight guard holds Steve
                               // (backstop only: Minecraft's FlightBrake slows Steve smoothly before that)
    float guardMinSpeed = 10.f; // blocks/s: below this the guard never acts (sprint-jumping is ~7-8)
    float guardMaxAbove = 11.f; // the guard holds only within this many blocks above known ground (MC's
                                // own hold needs ground within 12 blocks, else it lasts 6 s)
};
void SetFlightConfig(const FlightConfig& c);
FlightConfig GetFlightConfig();
// The effective lookahead (s) for motion m (0 without a valid motion or with lookahead off).
float Horizon(const Motion& m);
// Earliest time (s, within horizonS) at which m's straight-line path comes within `margin`
// blocks (horizontally) of region column (rx, rz); INFINITY if it doesn't.
float ColumnEta(int rx, int rz, const Motion& m, float horizonS, float margin);
// Every region column within `margin` of m's path over horizonS, with its ETA (packed Key(rx, rz)).
void CorridorColumns(const Motion& m, float horizonS, float margin, std::vector<std::pair<uint64_t, float>>& out);
inline uint64_t PackKey(int rx, int rz)
{
    return (static_cast<uint64_t>(static_cast<uint32_t>(rx)) << 32) | static_cast<uint32_t>(rz);
}
inline int KeyRx(uint64_t k)
{
    return static_cast<int32_t>(k >> 32);
}
inline int KeyRz(uint64_t k)
{
    return static_cast<int32_t>(k & 0xFFFFFFFFu);
}

// Scan planner (scanner side; shared by the native source and the offline tests).
struct PlanSettings
{
    int radius = 2, refreshMs = 1500, below = 12, above = 20;
};
struct PlanItem
{
    int rx, rz, ryLo, ryHi;
    float eta;   // s until the player is expected there (0 = here)
    bool urgent; // never scanned, or its vertical range isn't covered
    bool must;   // the player needs it within 0.5 s (unscanned, or not at the height it will be at):
                 // the scanner may go past its budget (up to CollisionBudgetMaxUs) for it
};
// Due columns around (cx, cy, cz) (MC feet) and along `m` (if valid), most urgent first:
// unscanned / range-moved columns by ETA, then the stalest. Also forgets far scan records.
// `focus` (optional, valid): also the columns along it (an ender pearl), at its own heights.
void PlanScan(double cx, double cy, double cz, const Motion& m, const PlanSettings& s, uint64_t nowMs, std::vector<PlanItem>& out,
              const Motion* focus = nullptr);
// Perf: the host's hold gate waits for W3 scans newer than `sinceMs` of the region
// columns within `radius` of (x, z) (MC coords). PlanScan then rescans those first (urgent) instead of
// at their periodic refresh: every return from a Witcher menu froze Steve for CollisionRefreshMs
// (1.5 s) in game. active = false: no gate waiting. Host thread sets, scanner reads.
void SetGate(bool active, double x, double z, int radius, uint64_t sinceMs);
void MarkScanned(int rx, int rz, int ryLo, int ryHi, bool terrain, uint64_t nowMs);

// ---- native PhysX source (collision_physx.cpp) ----------------------------------------------
// Offline/online resolver over a mapped witcher3.exe image (no execution; used by col_scan too).
struct Offsets
{
    uint8_t* npPhysicsInstance = nullptr; // &NpPhysics::mInstance (global NpPhysics*)
    uint32_t scenesArrayOff = 0x08;       // NpPhysics: Ps::Array<NpScene*> {data, size}
    uint32_t scenesCountOff = 0x10;
    uint32_t raycastSlot = 0;             // PxScene vtable byte offset of raycast (overlap = +0x10)
    uint32_t overlapSlot = 0;
    uint32_t worldSceneOff = 0;           // CPhysicsWorldPhysXImpl: PxScene*
    uint32_t worldBusyOff = 0;            // ...: "scene not queryable" flag (W3 returns "busy")
    uint32_t worldQueryCountOff = 0;      // ...: in-flight query counter (lock inc/dec)
    uint8_t* groupRegistry = nullptr;     // &(global ptr to the collision-group CName list)
    uint8_t* staticTraceNative = nullptr; // for the log
};
bool ResolveImage(const pe::Module& m, Offsets& o);
void DumpOffsets(const pe::Module& m, const Offsets& o);

// Live process: resolve + ini. Safe to call more than once. False = native source off.
bool InitNative();
// Game thread (script native). Scans region columns around (w3x, w3y, w3z) (W3 coords: Minecraft's
// player, see host::SampleCentre) within the time budget. Returns columns scanned (-1 = disabled).
int Scan(float w3x, float w3y, float w3z);
void SetEnabled(bool on);
std::string NativeStatus();
// WitcherCraft's invisible collision boxes for Steve's blocks (blocks.hpp: 1 m, centred in the
// block, so the Witcher's NPCs bump into his builds) are left out of the scan: Minecraft's own block
// is Steve's collision there. The source fills sorted blocks::CellKey keys of the cells that have one;
// the DLL registers blocks::StandInCells. Without a source (offline tools) nothing is skipped.
using StandInSource = void (*)(std::vector<uint64_t>& sortedKeys);
void SetStandInSource(StandInSource fn);
} // namespace wc::collision
