#pragma once
// WitcherCraft M2b: Minecraft blocks shown in The Witcher 3.
//
// The host thread feeds every render-ring message from Minecraft into OnRenderMessage(). We
// keep, per 16^3 section, which blocks are solid (kRenSolids) and which have at least one face in
// Minecraft's mesh (kRenSection), plus a colour class per block from its face's atlas texels
// (kRenAtlas) times the vertex tint. A block is "visible" if it is solid AND has a face, so fully
// buried blocks, water, plants and torches are not shown.
//
// Script polls deltas (Poll) and spawns/destroys one engine entity per visible block: an invisible
// 1 m collision box, so the Witcher's NPCs can't walk through Steve's builds. Each spawned block gets
// a small integer id (dense, reused) so the script can keep its entities in a plain array indexed by
// id. The number of spawned blocks is capped (kMaxSpawned); the rest wait. Steve's own collision
// leaves the boxes out (StandInCells, collision::SetStandInSource): Minecraft's block is his
// collision there, and a box would outlive a broken block for a moment.
#include <cstdint>
#include <string>
#include <vector>

namespace skycraft::proto
{
struct McState;
}

namespace wc::blocks
{
// Colour classes (from the early days, when each mapped to a visible stand-in; every block is the same
// invisible box now).
enum BlockClass : uint8_t
{
    kStone = 0,  // grey, low saturation (stone, cobblestone, andesite, gravel, ores, iron...)
    kLight = 1,  // bright and pale or sandy (sand, sandstone, birch, quartz, snow, white wool...)
    kWood = 2,   // brown / orange (planks, logs, crafting table, chests, dirt-ish browns)
    kDark = 3,   // dark (obsidian, deepslate, blackstone, dark oak, coal...)
    kGreen = 4,  // green hues (grass block top, leaves, moss, emerald)
    kVivid = 5,  // any other saturated colour (bricks, red/blue/yellow wool, gold, lapis, TNT...)
    kClassCount
};

constexpr int kMaxSpawned = 2000;

// Host thread: one render-ring message. Must stay cheap (never blocks on the script).
void OnRenderMessage(uint32_t type, const uint8_t* payload, uint32_t bytes);
// Host thread: Minecraft (re)connected. Everything it had sent is gone (it resends).
void OnMcReconnect();

// Script thread. Up to maxItems deltas, in W3 coordinates of the block's minimum corner
// (the block occupies [x,x+1] x [y,y+1] x [z,z+1] in W3 metres):
//   "+id,x,y,z,class;"   spawn block `id`
//   "-id;"               destroy block `id`
std::string Poll(int maxItems);
// Script thread: the script destroyed every block entity (world change, script restart).
// Every currently visible block will be re-announced with "+..." by the next polls.
void ResetSpawned();

// MC block cell -> key (x, z 24-bit signed, y 16-bit signed). Used by StandInCells.
inline uint64_t CellKey(int x, int y, int z)
{
    return (static_cast<uint64_t>(static_cast<uint32_t>(x) & 0xFFFFFFu) << 40) |
           (static_cast<uint64_t>(static_cast<uint32_t>(z) & 0xFFFFFFu) << 16) | (static_cast<uint32_t>(y) & 0xFFFFu);
}

// Any thread. Sorted CellKeys of the cells with a collision box: spawned, or removed less than
// kStandInGraceMs ago (the game may hold a destroyed entity's physics for a moment).
constexpr uint64_t kStandInGraceMs = 3000;
void StandInCells(std::vector<uint64_t>& sortedKeys);

// Nearest visible block hit along the ray (MC coords), at most maxT along dir (unit vector).
// Returns false if none.
bool Raycast(const double origin[3], const double dir[3], double maxT, double& tHit);

struct Stats
{
    size_t sections = 0, visible = 0, spawned = 0, pending = 0, overflow = 0, buried = 0;
    uint64_t sectionMsgs = 0, solidMsgs = 0, atlasMsgs = 0, clears = 0, adds = 0, removes = 0;
    uint32_t atlasW = 0, atlasH = 0;
    bool capHit = false;
};
Stats GetStats();
std::string StatusSuffix(); // " | blocks vis=... spawned=..."

// MC block coords -> W3 min corner (for logs / tests).
inline void McBlockToW3(int bx, int by, int bz, int& wx, int& wy, int& wz)
{
    wx = bx;
    wy = -bz - 1;
    wz = by;
}
} // namespace wc::blocks
