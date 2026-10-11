#pragma once
// SkyCraft-protocol HOST for The Witcher 3 (M1 "mirror mode").
//
// Creates the shared mapping `Local\SkyCraft_v1` exactly like SkyCraft's SKSE plugin
// (skse/src/Link.cpp) so the unchanged SkyCraft Fabric mod links to it. A background thread
// heartbeats, publishes SkyState at ~60 Hz, drains the MC->host rings and streams a terrain
// height field into the collision ring. Scripts only push data (cheap, main thread).
//
// Coordinates: W3 (x east, y north, z up, metres) ->
// MC (x = W3.x, y = W3.z, z = -W3.y), 1 block = 1 m. MC yaw = 180 - W3 heading (deg).
//
// M3a: when control::Driven() (Minecraft mode, see control.hpp) the host stops mirror-following:
// Minecraft's physics moves its player, the look comes from control::GetLook, and Minecraft is
// teleported only on a load / world change / W3-side jump / explicit request / safety rescue.
// Every such teleport is HELD (kSkyLoading published) until the collision around the target has
// been streamed and consumed by Minecraft ("hold gate"); a safety net puts a Minecraft player
// that fell through the world back on the ground.
#include <cstdint>
#include <string>
#include <string_view>

namespace skycraft::proto
{
struct McState;
}

namespace wc::host
{
// Flags for Tick(). Bits 0-2 are passed straight through as SkyFlags.
enum TickFlags : uint32_t
{
    kTickInGame = 1u << 0,   // a save is loaded and the player exists (SkyFlags::kSkyInGame)
    kTickMenuOpen = 1u << 1, // a W3 menu is open (SkyFlags::kSkyMenuOpen)
    kTickLoading = 1u << 2,  // loading / teleport in progress (SkyFlags::kSkyLoading)
    kTickNoFollow = 1u << 8, // don't make MC follow every move (only explicit teleports)
    kTickTeleport = 1u << 9, // force a teleport of the MC player to this position now
};

struct Status
{
    bool mapped = false;
    bool mcAlive = false;      // MC heartbeat seen within 3 s
    uint32_t mcPid = 0;
    bool haveMcState = false;  // got a consistent McState snapshot
    uint32_t mcFlags = 0;      // McFlags
    double mcX = 0, mcY = 0, mcZ = 0; // MC feet (MC coords)
    float mcYaw = 0, mcPitch = 0;
    uint64_t mcFrame = 0;
    uint32_t teleportSeq = 0, teleportAck = 0;
    uint32_t epoch = 0;
    uint32_t skyFlags = 0;     // what we last published
    double skyX = 0, skyY = 0, skyZ = 0; // what we last published (MC coords)
    uint64_t teleports = 0;
    uint64_t columnsSent = 0, blocksSent = 0, trisSent = 0, ringFull = 0;
    uint64_t obstacleTrisSent = 0; // M5a: W3 wall/building triangles streamed
    uint64_t obsCorners = 0, obsCornersExplained = 0; // M5a: ground samples matched by a W3 floor triangle
    uint64_t renderBytesDrained = 0, eventsDrained = 0;
    size_t heightSamples = 0, dirtyColumns = 0;
    uint64_t ticks = 0;
    bool driven = false;        // M3a: Minecraft drives the player
    bool gateHolding = false;   // a teleport waits for collision around its target
    uint64_t gateHolds = 0, rescues = 0;
    // M6b flight
    double speed = 0;                  // Minecraft's player, blocks/s (from its ticks)
    uint64_t flightHolds = 0;          // flight guard held the player (path not ready)
    uint64_t guardSkips = 0;           // ... path not ready, but high above known ground: not held
    uint64_t partialSent = 0;          // columns sent ahead of the script heights (W3 data only)
    uint64_t aheadSent = 0;            // columns sent on the flight path beyond the normal radius
    double sendMsMax = 0;              // longest host iteration spent building columns
    // Perf: host time per collision column built, and ender pearls Minecraft held
    // in the air waiting for collision (episodes, longest, total).
    uint64_t sendCols = 0;
    double sendColMsSum = 0, sendColMsMax = 0;
    uint64_t pearls = 0, pearlHolds = 0;
    double pearlHeldMsMax = 0, pearlHeldMsSum = 0;
};

// Creates the mapping and starts the host thread. Idempotent; false if the mapping failed.
bool Start();
bool Started();
// Stops the thread (offline tools only; the game never calls this).
void Stop();

// Player state from script, W3 coordinates. Call at 20-60 Hz. Flags: TickFlags.
void Tick(float w3x, float w3y, float w3z, float headingDeg, float pitchDeg, uint32_t flags);
// One ground sample at W3 integer lattice point (wx, wy). groundZ NaN = no ground there.
void PushHeight(int wx, int wy, float groundZ);
// Batched: "x,y,h;x,y,h;..." (W3 coords; h may be "n" for no ground). Returns samples parsed.
int PushHeights(std::string_view text);
// World identity (e.g. world depot path). A change wipes MC's collision (new epoch) and
// the height cache, and teleports the MC player.
void SetWorld(std::string_view name);

// Overlay (HUD) support, owned by the renderer side. The back-buffer size MC should render its
// HUD overlay at (0 = no overlay; MC keeps its own window size). Thread-safe.
void SetViewport(uint32_t width, uint32_t height);
// Base of the shared mapping (nullptr before Start()). For the overlay reader only.
uint8_t* MappingBase();

// Ground height (MC y) the script sampled at MC lattice point (mcx, mcz); NaN = no ground.
// False if never sampled (or pruned). Thread-safe. Used by blocks.cpp (M2b).
bool GroundAt(int mcx, int mcz, float& mcy);

// Consistent McState snapshot straight from the mapping (any thread). False if unmapped / torn.
bool ReadMc(skycraft::proto::McState& out);
// Where the script should sample ground heights (W3 coords): the pending teleport target, else
// Minecraft's feet while it drives, else the W3 player. False before the first tick.
bool SampleCentre(float& w3x, float& w3y, float& w3z);
// M6b: the same centre without the flight lead (Minecraft's feet, or the teleport target): where
// the native collision scanner centres its square.
bool ScanCentre(float& w3x, float& w3y, float& w3z);

Status GetStatus();
std::string StatusLine();      // one line for on-screen debug
} // namespace wc::host
