// SkyCraft-protocol host for The Witcher 3. See skycraft_host.hpp.
//
// Threads:
//   - script/main thread: Tick / PushHeight(s) / SetWorld / GetStatus. Only touch g_in under g_mutex.
//   - host thread (~60 Hz): everything that touches shared memory. Never blocks on Minecraft.
#include "skycraft_host.hpp"

#include "blocks.hpp"
#include "buildmode.hpp"
#include "collision.hpp"
#include "combat.hpp"
#include "control.hpp"
#include "log.hpp"
#include "skycraft_protocol.h"

#include <Windows.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace wc::host
{
namespace proto = skycraft::proto;

namespace
{
// ---- tunables ---------------------------------------------------------------------------
constexpr uint64_t kMcTimeoutMs = 3000;      // MC heartbeat older than this = MC gone (as Link.cpp)
constexpr uint64_t kStaleTickMs = 750;       // no script Tick for this long = W3 paused/loading
constexpr double kFollowEps = 0.05;          // blocks; smaller moves don't re-teleport MC
constexpr uint64_t kFollowIntervalMs = 50;   // at most one follow teleport per MC tick
constexpr int kRegion = 8;                   // must match SkyCollision.REGION_SIZE
constexpr int kColumnRadius = 3;             // stream region columns within +-3 regions of the player
constexpr float kCrust = 2.0f;               // blocks of solid voxels under the surface
constexpr int kColumnsPerIteration = 6;      // collision columns built per host iteration
constexpr int kPruneDistance = 96;           // blocks; height samples further away are dropped
constexpr DWORD kPeriodMs = 16;              // host loop period (~60 Hz)
// M3a (Minecraft drives)
constexpr uint64_t kGateTimeoutMs = 15000;   // a held teleport goes ahead anyway after this
constexpr int kGateRadius = 1;               // region columns around the target that must be sent (>= 8 blocks)
constexpr double kRescueBelowGround = 4.0;   // MC feet this far under the sampled ground = fell through
constexpr double kRescueBelowMin = 10.0;     // ... or this far under the lowest sampled ground nearby
constexpr uint64_t kRescueIntervalMs = 3000; // at most one rescue per this
constexpr double kW3JumpBlocks = 6.0;        // W3 moved its player this far in one step: resync MC
constexpr double kStayBlocks = 3.0;          // re-place MC where it is if the W3 player is this close

template <class T>
std::atomic_ref<T> A(T& v)
{
    return std::atomic_ref<T>(v);
}

int FloorDiv(int v, int d)
{
    return v >= 0 ? v / d : -((-v + d - 1) / d);
}

uint64_t Key(int a, int b)
{
    return (static_cast<uint64_t>(static_cast<uint32_t>(a)) << 32) | static_cast<uint32_t>(b);
}
int KeyA(uint64_t k)
{
    return static_cast<int32_t>(static_cast<uint32_t>(k >> 32));
}
int KeyB(uint64_t k)
{
    return static_cast<int32_t>(static_cast<uint32_t>(k));
}

// ---- mapping ----------------------------------------------------------------------------
HANDLE g_mapping = nullptr;
uint8_t* g_base = nullptr;

template <class T>
T* At(uint64_t off)
{
    return reinterpret_cast<T*>(g_base + off);
}

bool Elevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

// Same rules as SkyCraft's Link.cpp: SYSTEM, Administrators and THIS user get full access, at
// medium integrity ("ME" mandatory label, no-write-up), so a non-elevated Minecraft can open
// the mapping even if the game runs elevated. Free with LocalFree.
PSECURITY_DESCRIPTOR SharedWithThisUser()
{
    std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)";
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<uint8_t> buffer(size);
        if (size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size))
        {
            LPWSTR sid = nullptr;
            if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid))
            {
                sddl += std::wstring(L"(A;;GA;;;") + sid + L")";
                LocalFree(sid);
            }
        }
        CloseHandle(token);
    }
    sddl += L"S:(ML;;NW;;;ME)";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
    {
        WLOG("host: couldn't build the mapping's access rules (%lu); using defaults", GetLastError());
        return nullptr;
    }
    return descriptor;
}

bool CreateMapping()
{
    const uint64_t size = proto::kMappingBytes;
    if (Elevated())
        WLOG("host: game is running as administrator (mapping ACL keeps MC able to open it)");
    SECURITY_ATTRIBUTES access{sizeof(access), SharedWithThisUser(), FALSE};
    g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, access.lpSecurityDescriptor ? &access : nullptr, PAGE_READWRITE,
                                   static_cast<DWORD>(size >> 32), static_cast<DWORD>(size & 0xFFFFFFFF), proto::kMappingName);
    const DWORD created = GetLastError();
    if (access.lpSecurityDescriptor)
        LocalFree(access.lpSecurityDescriptor);
    if (!g_mapping)
    {
        WLOG("host: CreateFileMapping(%ls) failed (%lu)", proto::kMappingName, created);
        return false;
    }
    const bool existed = created == ERROR_ALREADY_EXISTS;
    g_base = static_cast<uint8_t*>(MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!g_base)
    {
        WLOG("host: MapViewOfFile failed (%lu)", GetLastError());
        CloseHandle(g_mapping);
        g_mapping = nullptr;
        return false;
    }
    // A stale mapping survives while MC keeps it open: reset everything the host owns, exactly
    // like Link::Create, so rings and the overlay swap start from a known state.
    auto* header = At<proto::Header>(proto::kOffHeader);
    std::memset(g_base + proto::kOffSkyState, 0, sizeof(proto::SkyState));
    std::memset(g_base + proto::kOffOverlayCtl, 0, 0x100);
    std::memset(g_base + proto::kOffInputRing, 0, proto::kInputRingDataOff);
    std::memset(g_base + proto::kOffCollisionRing, 0, proto::kColRingDataOff);
    std::memset(g_base + proto::kOffActorTable, 0, sizeof(proto::ActorTable));
    std::memset(g_base + proto::kOffEventRing, 0, proto::kEventRingDataOff);
    std::memset(g_base + proto::kOffWorldEntities, 0, sizeof(proto::WorldEntities));
    std::memset(g_base + proto::kOffRenderRing, 0, proto::kRenRingDataOff);
    std::memset(g_base + proto::kOffWaterGrid, 0, sizeof(proto::WaterGrid));
    header->version = proto::kVersion;
    header->skyrimPid = GetCurrentProcessId();
    header->skyrimHeartbeatMs = GetTickCount64();
    A(header->magic).store(proto::kMagic, std::memory_order_release);
    WLOG("host: shared memory %ls (%llu MB, %s), protocol v%u, pid %lu", proto::kMappingName,
         static_cast<unsigned long long>(size >> 20), existed ? "reused" : "created", proto::kVersion,
         GetCurrentProcessId());
    return true;
}

void Heartbeat()
{
    A(At<proto::Header>(proto::kOffHeader)->skyrimHeartbeatMs).store(GetTickCount64(), std::memory_order_release);
}

bool McAlive()
{
    const uint64_t last = A(At<proto::Header>(proto::kOffHeader)->mcHeartbeatMs).load(std::memory_order_acquire);
    return last != 0 && GetTickCount64() - last < kMcTimeoutMs;
}

uint32_t McPid()
{
    return A(At<proto::Header>(proto::kOffHeader)->mcPid).load(std::memory_order_acquire);
}

void WriteSkyState(const proto::SkyState& st)
{
    auto* dst = At<proto::SkyState>(proto::kOffSkyState);
    auto seq = A(dst->seq);
    const uint32_t s = seq.load(std::memory_order_relaxed);
    seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(reinterpret_cast<uint8_t*>(dst) + 4, reinterpret_cast<const uint8_t*>(&st) + 4, sizeof(proto::SkyState) - 4);
    seq.store(s + 2, std::memory_order_release);
}

bool ReadMcState(proto::McState& out)
{
    auto* src = At<proto::McState>(proto::kOffMcState);
    auto seq = A(src->seq);
    for (int attempt = 0; attempt < 64; ++attempt)
    {
        const uint32_t s1 = seq.load(std::memory_order_acquire);
        if (s1 & 1)
        {
            YieldProcessor();
            continue;
        }
        std::memcpy(&out, src, sizeof(proto::McState));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq.load(std::memory_order_relaxed) == s1)
            return true;
    }
    return false;
}

// Collision ring producer (host thread only). False if the ring is full; never waits.
bool WriteCollision(proto::ColType type, const void* payload, uint32_t bytes)
{
    auto* ring = g_base + proto::kOffCollisionRing;
    auto& headRef = *reinterpret_cast<uint64_t*>(ring + proto::kColRingHeadOff);
    auto& tailRef = *reinterpret_cast<uint64_t*>(ring + proto::kColRingTailOff);
    auto* data = ring + proto::kColRingDataOff;
    constexpr uint64_t size = proto::kColRingDataBytes;

    const uint64_t msgBytes = (sizeof(proto::ColMsgHeader) + bytes + 7) & ~7ull;
    if (msgBytes > size / 2)
    {
        WLOG("host: collision message too large (%llu bytes)", static_cast<unsigned long long>(msgBytes));
        return false;
    }
    uint64_t head = A(headRef).load(std::memory_order_relaxed);
    const uint64_t tail = A(tailRef).load(std::memory_order_acquire);
    uint64_t pos = head % size;
    const uint64_t padBytes = (pos + msgBytes > size) ? size - pos : 0;
    if (size - (head - tail) < msgBytes + padBytes)
        return false;
    if (padBytes)
    {
        *reinterpret_cast<proto::ColMsgHeader*>(data + pos) = {proto::kColPad, 0};
        head += padBytes;
        pos = 0;
    }
    *reinterpret_cast<proto::ColMsgHeader*>(data + pos) = {type, bytes};
    if (bytes)
        std::memcpy(data + pos + sizeof(proto::ColMsgHeader), payload, bytes);
    A(headRef).store(head + msgBytes, std::memory_order_release);
    return true;
}

// Render ring (MC -> host). MC's writeRender() waits up to 1 s for space when the ring is full,
// so it MUST be drained every iteration. Every message is handed to blocks::OnRenderMessage
// (which only keeps sections, solids and the atlas; cheap), exactly like SkyCraft's
// Link::DrainRender walks it. On a malformed header we resync by jumping to head (always a
// message boundary).
uint64_t DrainRenderRing()
{
    auto* ring = g_base + proto::kOffRenderRing;
    auto& headRef = *reinterpret_cast<uint64_t*>(ring + proto::kRenRingHeadOff);
    auto& tailRef = *reinterpret_cast<uint64_t*>(ring + proto::kRenRingTailOff);
    const uint64_t head = A(headRef).load(std::memory_order_acquire);
    uint64_t tail = A(tailRef).load(std::memory_order_relaxed);
    if (head == tail)
        return 0;
    const uint64_t start = tail;
    const uint8_t* data = ring + proto::kRenRingDataOff;
    constexpr uint64_t size = proto::kRenRingDataBytes;
    while (tail < head)
    {
        const uint64_t pos = tail % size;
        proto::ColMsgHeader hdr;
        std::memcpy(&hdr, data + pos, sizeof(hdr));
        if (hdr.type == proto::kRenPad)
        {
            tail += size - pos;
            continue;
        }
        const uint64_t msgBytes = (sizeof(proto::ColMsgHeader) + static_cast<uint64_t>(hdr.payloadBytes) + 7) & ~7ull;
        if (pos + msgBytes > size || tail + msgBytes > head)
        {
            static int warned = 0;
            if (warned++ < 5)
                WLOG("host: render ring desync (type %u, %u bytes at %llu); skipping to head", hdr.type, hdr.payloadBytes,
                     static_cast<unsigned long long>(tail));
            tail = head;
            break;
        }
        blocks::OnRenderMessage(hdr.type, data + pos + sizeof(proto::ColMsgHeader), hdr.payloadBytes);
        tail += msgBytes;
    }
    A(tailRef).store(tail, std::memory_order_release);
    return tail - start;
}

// Event ring (MC -> host): MC drops events when it's full (never blocks). Every event goes to
// combat.cpp (M4a), which converts hits / deaths / explosions for the script.
uint64_t DrainEventRing()
{
    auto* ring = g_base + proto::kOffEventRing;
    auto& headRef = *reinterpret_cast<uint64_t*>(ring + proto::kEventRingHeadOff);
    auto& tailRef = *reinterpret_cast<uint64_t*>(ring + proto::kEventRingTailOff);
    const uint64_t head = A(headRef).load(std::memory_order_acquire);
    uint64_t tail = A(tailRef).load(std::memory_order_relaxed);
    if (tail >= head)
        return 0;
    if (head - tail > proto::kEventRingEntries)
        tail = head - proto::kEventRingEntries;
    uint64_t n = 0;
    static int logged = 0;
    for (; tail < head; ++tail, ++n)
    {
        const auto& ev = reinterpret_cast<const proto::McEvent*>(ring + proto::kEventRingDataOff)[tail & (proto::kEventRingEntries - 1)];
        if (logged < 50)
        {
            ++logged;
            WLOG("host: MC event type=%u formId=%d a=%.2f b=%.2f c=%.2f d=%.2f", ev.type, static_cast<int32_t>(ev.formId), ev.a, ev.b, ev.c, ev.d);
        }
        combat::OnMcEvent(ev);
    }
    A(tailRef).store(head, std::memory_order_release);
    return n;
}

// ---- shared state between script thread and host thread ---------------------------------
struct Input
{
    bool haveTick = false;
    uint64_t lastTickMs = 0;
    double x = 0, y = 0, z = 0; // MC coords
    float yaw = 0, pitch = 0;   // MC degrees
    uint32_t flags = 0;
    bool teleportRequested = false;
    uint32_t worldId = 0;
    uint64_t worldGen = 0;      // bumps on SetWorld change
};

std::mutex g_mutex;           // guards g_in, g_heights, g_dirty
Input g_in;
std::unordered_map<uint64_t, float> g_heights; // (mcX, mcZ) lattice -> MC y; NaN = no ground
std::unordered_map<uint64_t, float> g_water;   // M7: (mcX, mcZ) lattice -> MC y of W3's water surface (only where there is water)
std::unordered_set<uint64_t> g_dirty;          // (rx, rz) region columns to (re)send

std::mutex g_statusMutex;
Status g_status;

std::atomic<bool> g_started{false};
std::atomic<bool> g_stop{false};
std::atomic<uint32_t> g_viewportW{0}, g_viewportH{0};
// Heap-allocated and never destroyed: a global std::thread still joinable at process exit
// calls std::terminate (0xC0000409 when W3 quits). Stop() joins and frees it.
std::thread* g_thread = nullptr;
std::once_flag g_once;
bool g_startOk = false;

// Marks every region column whose 9x9 corner set includes lattice point (x, z). Caller holds g_mutex.
void MarkDirtyCorner(int x, int z)
{
    const int rx = FloorDiv(x, kRegion), rz = FloorDiv(z, kRegion);
    const bool edgeX = x - rx * kRegion == 0, edgeZ = z - rz * kRegion == 0;
    g_dirty.insert(Key(rx, rz));
    if (edgeX)
        g_dirty.insert(Key(rx - 1, rz));
    if (edgeZ)
        g_dirty.insert(Key(rx, rz - 1));
    if (edgeX && edgeZ)
        g_dirty.insert(Key(rx - 1, rz - 1));
}

void StoreHeight(int mcx, int mcz, float mcy) // caller holds g_mutex
{
    auto [it, inserted] = g_heights.try_emplace(Key(mcx, mcz), mcy);
    if (!inserted)
    {
        const float old = it->second;
        const bool same = (std::isnan(old) && std::isnan(mcy)) || (!std::isnan(old) && !std::isnan(mcy) && std::fabs(old - mcy) < 0.02f);
        if (same)
            return;
        it->second = mcy;
    }
    MarkDirtyCorner(mcx, mcz);
}

// M7 swimming: W3's water around (x, z) (MC coords) -> the WaterGrid Minecraft swims in (SkyWater).
// A block column is water where any of its 4 lattice corners has a W3 water surface (the script
// samples GetWaterLevel together with the ground). Host thread only.
void WriteWaterGrid(double x, double z, uint32_t worldId)
{
    constexpr int N = static_cast<int>(proto::kWaterGridSize);
    float surface[N * N];
    const int ox = static_cast<int>(std::floor(x)) - N / 2, oz = static_cast<int>(std::floor(z)) - N / 2;
    {
        std::lock_guard lk(g_mutex);
        for (int j = 0; j < N; ++j)
            for (int i = 0; i < N; ++i)
            {
                float s = proto::kNoWater;
                for (int dx = 0; dx <= 1; ++dx)
                    for (int dz = 0; dz <= 1; ++dz)
                    {
                        auto it = g_water.find(Key(ox + i + dx, oz + j + dz));
                        if (it != g_water.end())
                            s = std::max(s, it->second);
                    }
                surface[j * N + i] = s;
            }
    }
    auto* dst = At<proto::WaterGrid>(proto::kOffWaterGrid);
    auto seq = A(dst->seq);
    const uint32_t s = seq.load(std::memory_order_relaxed);
    seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    dst->originX = ox;
    dst->originZ = oz;
    dst->worldId = worldId;
    std::memcpy(dst->surface, surface, sizeof(surface));
    seq.store(s + 2, std::memory_order_release);
}

// ---- host-thread state ------------------------------------------------------------------
struct ColumnState
{
    uint64_t hash = 0;
    int ryLo = 0, ryHi = -1; // regions currently holding our data (empty if hi < lo)
    bool processed = false;  // built from a complete corner set at least once (may be all "no ground")
    uint64_t obsVersion = 0; // M5a: collision-store version of the obstacle data last sent
    bool complete = false;   // M6b: sent with every script height corner (else only W3's own data)
    uint64_t ringHead = 0;   // M6b: collision-ring head after it was written (consumed when tail >= this)
    int obsRyLo = 0, obsRyHi = -1; // heights covered by the W3 scan last sent (empty if hi < lo)
};
std::unordered_map<uint64_t, ColumnState> g_sent; // host thread only

uint64_t HashCorners(const float (&h)[kRegion + 1][kRegion + 1])
{
    uint64_t hash = 1469598103934665603ull;
    for (auto& row : h)
        for (float v : row)
        {
            const int32_t q = std::isnan(v) ? INT32_MIN : static_cast<int32_t>(std::lround(v * 100.0f));
            hash = (hash ^ static_cast<uint32_t>(q)) * 1099511628211ull;
        }
    return hash;
}

// Height of the cell's two-triangle surface at (u, v) in [0,1]^2. The split runs from corner
// (0,0) to (1,1), matching the triangles we send.
float CellHeight(float h00, float h10, float h01, float h11, float u, float v)
{
    return u >= v ? h00 + u * (h10 - h00) + v * (h11 - h10) : h00 + v * (h01 - h00) + u * (h11 - h01);
}

// MC height of triangle t at (px, pz) if (px, pz) lies inside its XZ projection (edges included).
bool TriHeightAt(const proto::ColTri& t, float px, float pz, float& y)
{
    const float ax = t.v[0], az = t.v[2], bx = t.v[3], bz = t.v[5], cx = t.v[6], cz = t.v[8];
    const float det = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz);
    if (std::fabs(det) < 1e-6f)
        return false;
    const float l1 = ((bz - cz) * (px - cx) + (cx - bx) * (pz - cz)) / det;
    const float l2 = ((cz - az) * (px - cx) + (ax - cx) * (pz - cz)) / det;
    const float l3 = 1.0f - l1 - l2;
    constexpr float e = -1e-3f;
    if (l1 < e || l2 < e || l3 < e)
        return false;
    y = l1 * t.v[1] + l2 * t.v[4] + l3 * t.v[7];
    return true;
}

bool Walkable(const proto::ColTri& t)
{
    const float ux = t.v[3] - t.v[0], uy = t.v[4] - t.v[1], uz = t.v[5] - t.v[2];
    const float vx = t.v[6] - t.v[0], vy = t.v[7] - t.v[1], vz = t.v[8] - t.v[2];
    const float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    return len > 1e-8f && std::fabs(ny) / len >= 0.7f;
}

// Marks the 1/8-block sub-voxels the triangle passes through (a thin shell) in a region's dense
// voxel grid (dense[(i * 8 + by) * 8 + k] = block (i, by, k) of the region at (x0, y0, z0)).
void VoxelizeTri(const proto::ColTri& t, int x0, int y0, int z0, proto::ColBlock* dense, bool* used)
{
    auto edge = [&](int a, int b) {
        const float dx = t.v[b * 3] - t.v[a * 3], dy = t.v[b * 3 + 1] - t.v[a * 3 + 1], dz = t.v[b * 3 + 2] - t.v[a * 3 + 2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    const float longest = std::max({edge(0, 1), edge(1, 2), edge(2, 0)});
    const int n = std::clamp(static_cast<int>(std::ceil(longest * 16.0f)), 1, 160); // 1/16-block steps
    // Perf: only the samples that can land in THIS region are visited. A sample is
    // p(i, j) = v0 + (i/n) e1 + (j/n) e2; for each i the j's inside the region's box (+1/8 block, and
    // one extra step on both sides) form one interval. The per-sample test below still decides, so
    // the voxels are exactly the ones the full n^2 sweep set (big wall triangles binned into several
    // regions, or reaching far out of the column, were swept whole once per region: tens of ms per
    // column in Novigrad).
    const double lo[3] = {static_cast<double>(x0) - 0.125, static_cast<double>(y0) - 0.125, static_cast<double>(z0) - 0.125};
    const double hi[3] = {static_cast<double>(x0 + kRegion) + 0.125, static_cast<double>(y0 + kRegion) + 0.125,
                          static_cast<double>(z0 + kRegion) + 0.125};
    for (int i = 0; i <= n; ++i)
    {
        const double ad = static_cast<double>(i) / n;
        double jLo = 0.0, jHi = static_cast<double>(n - i);
        for (int c = 0; c < 3 && jLo <= jHi; ++c)
        {
            const double base = t.v[c] + (static_cast<double>(t.v[3 + c]) - t.v[c]) * ad;
            const double e2 = (static_cast<double>(t.v[6 + c]) - t.v[c]) / n; // per j step
            if (std::fabs(e2) < 1e-9)
            {
                if (base < lo[c] || base > hi[c])
                    jHi = -1.0;
                continue;
            }
            double j0 = (lo[c] - base) / e2, j1 = (hi[c] - base) / e2;
            if (j0 > j1)
                std::swap(j0, j1);
            jLo = std::max(jLo, j0);
            jHi = std::min(jHi, j1);
        }
        static const bool fullSweep = std::getenv("WC_VOX_FULL") != nullptr; // A/B knob for the offline perf runs
        if (!(jLo <= jHi) && !fullSweep) // (also NaN)
            continue;
        const int jFirst = fullSweep ? 0 : std::max(0, static_cast<int>(std::floor(jLo)) - 1);
        const int jLast = fullSweep ? n - i : std::min(n - i, static_cast<int>(std::ceil(jHi)) + 1);
        for (int j = jFirst; j <= jLast; ++j)
        {
            const float a = static_cast<float>(i) / n, b = static_cast<float>(j) / n;
            float p[3];
            for (int c = 0; c < 3; ++c)
                p[c] = t.v[c] + (t.v[3 + c] - t.v[c]) * a + (t.v[6 + c] - t.v[c]) * b;
            const int sx = static_cast<int>(std::floor((p[0] - x0) * 8.0f)), sy = static_cast<int>(std::floor((p[1] - y0) * 8.0f)),
                      sz = static_cast<int>(std::floor((p[2] - z0) * 8.0f));
            if (sx < 0 || sy < 0 || sz < 0 || sx >= kRegion * 8 || sy >= kRegion * 8 || sz >= kRegion * 8)
                continue;
            const int idx = ((sx >> 3) * kRegion + (sy >> 3)) * kRegion + (sz >> 3);
            dense[idx].bits[sy & 7] |= 1ull << ((sz & 7) * 8 + (sx & 7));
            used[idx] = true;
        }
    }
}

// Builds and writes kColTris + kColRegion for every vertical region of column (rx, rz): the
// script's terrain height field plus (M5a) the W3 obstacle triangles from the collision store.
// Returns false if the ring was full (the column stays dirty and is retried).
bool SendColumn(int rx, int rz, const float (&h)[kRegion + 1][kRegion + 1], uint32_t epoch, ColumnState& state, Status& counters,
                const collision::ColumnInfo* obs)
{
    float minH = INFINITY, maxH = -INFINITY;
    for (auto& row : h)
        for (float v : row)
            if (!std::isnan(v))
            {
                minH = std::min(minH, v);
                maxH = std::max(maxH, v);
            }
    int newLo = 0, newHi = -1;
    if (minH <= maxH)
    {
        newLo = FloorDiv(static_cast<int>(std::floor(minH - kCrust - 9.0f)), kRegion);
        newHi = FloorDiv(static_cast<int>(std::floor(maxH + 4.0f)), kRegion);
        newHi = std::min(newHi, newLo + 15); // sanity cap: 128 blocks of relief per column
    }
    const bool haveObs = obs && obs->ryHi >= obs->ryLo;
    if (haveObs)
    {
        if (newHi < newLo)
            newLo = obs->ryLo, newHi = obs->ryHi;
        else
            newLo = std::min(newLo, obs->ryLo), newHi = std::max(newHi, obs->ryHi);
        newHi = std::min(newHi, newLo + 23);
    }
    int lo = newLo, hi = newHi;
    if (state.ryHi >= state.ryLo)
    {
        if (hi < lo)
        {
            lo = state.ryLo;
            hi = state.ryHi;
        }
        else
        {
            lo = std::min(lo, state.ryLo);
            hi = std::max(hi, state.ryHi);
        }
    }

    const int x0 = rx * kRegion, z0 = rz * kRegion;
    // M5a: the obstacle triangles per region, and which height-field corners they explain (the
    // script's ground ray hit one of them there: a floor, a street, a crate, a roof...). A
    // height-field cell whose 4 corners are all explained is dropped: its two triangles would only
    // be a coarse 1 m copy of that geometry, with ramps down its edges (walking up crates).
    std::vector<std::vector<proto::ColTri>> obsTris(hi >= lo ? static_cast<size_t>(hi - lo + 1) : 0);
    bool explained[kRegion + 1][kRegion + 1] = {};
    size_t obsCount = 0;
    if (haveObs)
    {
        for (int ry = lo; ry <= hi; ++ry)
        {
            collision::AppendRegion(rx, ry, rz, obsTris[ry - lo]);
            obsCount += obsTris[ry - lo].size();
        }
        for (auto& list : obsTris)
            for (const auto& t : list)
            {
                if (!Walkable(t))
                    continue;
                const float mnx = std::min({t.v[0], t.v[3], t.v[6]}), mxx = std::max({t.v[0], t.v[3], t.v[6]});
                const float mnz = std::min({t.v[2], t.v[5], t.v[8]}), mxz = std::max({t.v[2], t.v[5], t.v[8]});
                const int i0 = std::max(0, static_cast<int>(std::ceil(mnx - x0 - 1e-3f)));
                const int i1 = std::min(kRegion, static_cast<int>(std::floor(mxx - x0 + 1e-3f)));
                const int k0 = std::max(0, static_cast<int>(std::ceil(mnz - z0 - 1e-3f)));
                const int k1 = std::min(kRegion, static_cast<int>(std::floor(mxz - z0 + 1e-3f)));
                for (int i = i0; i <= i1; ++i)
                    for (int k = k0; k <= k1; ++k)
                    {
                        float y;
                        if (!explained[i][k] && !std::isnan(h[i][k]) &&
                            TriHeightAt(t, static_cast<float>(x0 + i), static_cast<float>(z0 + k), y) && std::fabs(y - h[i][k]) < 0.35f)
                            explained[i][k] = true;
                    }
            }
        for (int i = 0; i <= kRegion; ++i)
            for (int k = 0; k <= kRegion; ++k)
                if (!std::isnan(h[i][k]))
                {
                    counters.obsCorners++;
                    counters.obsCornersExplained += explained[i][k] ? 1 : 0;
                }
    }

    std::vector<uint8_t> payload;
    std::vector<proto::ColTri> tris;
    std::vector<proto::ColBlock> blocks;
    std::vector<proto::ColBlock> dense(kRegion * kRegion * kRegion);
    bool used[kRegion * kRegion * kRegion];
    uint64_t triCount = 0, blockCount = 0;
    for (int ry = lo; ry <= hi; ++ry)
    {
        const bool inRange = ry >= newLo && ry <= newHi;
        const int y0 = ry * kRegion;
        tris.clear();
        blocks.clear();
        std::memset(dense.data(), 0, dense.size() * sizeof(proto::ColBlock));
        std::memset(used, 0, sizeof(used));
        if (inRange)
        {
            const float yLo = static_cast<float>(y0) - 0.5f, yHi = static_cast<float>(y0 + kRegion) + 0.5f;
            for (int i = 0; i < kRegion; ++i)
                for (int k = 0; k < kRegion; ++k)
                {
                    const float h00 = h[i][k], h10 = h[i + 1][k], h01 = h[i][k + 1], h11 = h[i + 1][k + 1];
                    if (std::isnan(h00) || std::isnan(h10) || std::isnan(h01) || std::isnan(h11))
                        continue;
                    const float fx = static_cast<float>(x0 + i), fz = static_cast<float>(z0 + k);
                    const float cmin = std::min(std::min(h00, h10), std::min(h01, h11));
                    const float cmax = std::max(std::max(h00, h10), std::max(h01, h11));
                    const bool covered = explained[i][k] && explained[i + 1][k] && explained[i][k + 1] && explained[i + 1][k + 1];
                    // Triangles (exact surface for the player's smooth collider).
                    if (!covered && cmax >= yLo && cmin <= yHi)
                    {
                        proto::ColTri a{{fx, h00, fz, fx + 1, h10, fz, fx + 1, h11, fz + 1}, proto::kTriTerrain};
                        proto::ColTri b{{fx, h00, fz, fx + 1, h11, fz + 1, fx, h01, fz + 1}, proto::kTriTerrain};
                        tris.push_back(a);
                        tris.push_back(b);
                    }
                    // Voxels: a kCrust-thick solid layer under the surface (other entities, the
                    // "ground arrived" check in MC's holdUntilReady).
                    for (int by = 0; by < kRegion; ++by)
                    {
                        const float bottom = static_cast<float>(y0 + by);
                        if (bottom >= cmax || bottom + 1.0f <= cmin - kCrust)
                            continue;
                        const int idx = (i * kRegion + by) * kRegion + k;
                        proto::ColBlock& blk = dense[idx];
                        for (int sx = 0; sx < 8; ++sx)
                            for (int sz = 0; sz < 8; ++sz)
                            {
                                const float surf = CellHeight(h00, h10, h01, h11, (sx + 0.5f) / 8.0f, (sz + 0.5f) / 8.0f);
                                for (int sy = 0; sy < 8; ++sy)
                                {
                                    const float yc = bottom + (sy + 0.5f) / 8.0f;
                                    if (yc < surf && yc > surf - kCrust)
                                    {
                                        blk.bits[sy] |= 1ull << (sz * 8 + sx);
                                        used[idx] = true;
                                    }
                                }
                            }
                    }
                }
            // M5a: W3 walls, floors, roofs, rocks... (triangles + a 1/8-block voxel shell).
            if (haveObs)
                for (const auto& t : obsTris[ry - lo])
                {
                    tris.push_back(t);
                    VoxelizeTri(t, x0, y0, z0, dense.data(), used);
                }
        }
        for (int i = 0; i < kRegion; ++i)
            for (int by = 0; by < kRegion; ++by)
                for (int k = 0; k < kRegion; ++k)
                {
                    const int idx = (i * kRegion + by) * kRegion + k;
                    if (!used[idx])
                        continue;
                    proto::ColBlock blk = dense[idx];
                    blk.x = x0 + i;
                    blk.y = y0 + by;
                    blk.z = z0 + k;
                    blocks.push_back(blk);
                }

        proto::ColRegion reg{};
        reg.minX = x0;
        reg.minY = y0;
        reg.minZ = z0;
        reg.maxX = x0 + kRegion - 1;
        reg.maxY = y0 + kRegion - 1;
        reg.maxZ = z0 + kRegion - 1;
        reg.epoch = epoch;

        // Triangles first (as SkyCraft does), then the voxel region that marks it "known".
        reg.count = static_cast<uint32_t>(tris.size());
        payload.resize(sizeof(reg) + tris.size() * sizeof(proto::ColTri));
        std::memcpy(payload.data(), &reg, sizeof(reg));
        if (!tris.empty())
            std::memcpy(payload.data() + sizeof(reg), tris.data(), tris.size() * sizeof(proto::ColTri));
        if (!WriteCollision(proto::kColTris, payload.data(), static_cast<uint32_t>(payload.size())))
            return false;

        reg.count = static_cast<uint32_t>(blocks.size());
        payload.resize(sizeof(reg) + blocks.size() * sizeof(proto::ColBlock));
        std::memcpy(payload.data(), &reg, sizeof(reg));
        if (!blocks.empty())
            std::memcpy(payload.data() + sizeof(reg), blocks.data(), blocks.size() * sizeof(proto::ColBlock));
        if (!WriteCollision(proto::kColRegion, payload.data(), static_cast<uint32_t>(payload.size())))
            return false;
        triCount += tris.size();
        blockCount += blocks.size();
    }
    state.ryLo = newLo;
    state.ryHi = newHi;
    counters.columnsSent++;
    counters.trisSent += triCount;
    counters.blocksSent += blockCount;
    counters.obstacleTrisSent += obsCount;
    return true;
}

// M6b: the collision ring's head (ours) and tail (Minecraft's).
uint64_t RingHead()
{
    return A(*reinterpret_cast<uint64_t*>(g_base + proto::kOffCollisionRing + proto::kColRingHeadOff)).load(std::memory_order_acquire);
}
uint64_t RingTail()
{
    return A(*reinterpret_cast<uint64_t*>(g_base + proto::kOffCollisionRing + proto::kColRingTailOff)).load(std::memory_order_acquire);
}

// M6b: a column the player reaches within this many seconds is sent as soon as W3's own collision
// (PhysX, incl. its terrain height field) has been scanned, without waiting for every script
// height corner (the script's 96 traces per step can't keep up at elytra speed).
constexpr float kPartialEtaS = 2.0f; // (was 1.0: elytra flight outran it, see FlightBrake)
constexpr double kSendBudgetMs = 8.0; // host time per iteration for building columns

void StreamCollision(double px, double pz, uint32_t epoch, Status& counters, const collision::Motion& mo)
{
    const int prx = FloorDiv(static_cast<int>(std::floor(px)), kRegion);
    const int prz = FloorDiv(static_cast<int>(std::floor(pz)), kRegion);
    const float horizon = collision::Horizon(mo);
    const double slow = std::max(mo.valid ? mo.Speed() : 0.0, 4.0);
    // M6b: the flight corridor ahead (ETA per column), on top of the square around the player.
    std::unordered_map<uint64_t, float> corridor;
    if (horizon > 0.0f)
    {
        std::vector<std::pair<uint64_t, float>> cor;
        collision::CorridorColumns(mo, horizon, 8.0f, cor);
        for (auto& c : cor)
            corridor[Key(collision::KeyRx(c.first), collision::KeyRz(c.first))] = c.second;
    }
    collision::Motion pearl;
    if (collision::GetFocus(pearl, GetTickCount64()))
    {
        std::vector<std::pair<uint64_t, float>> cor;
        collision::CorridorColumns(pearl, 1.5f, 8.0f, cor);
        for (auto& c : cor)
        {
            auto [it, ins] = corridor.try_emplace(Key(collision::KeyRx(c.first), collision::KeyRz(c.first)), c.second);
            if (!ins)
                it->second = std::min(it->second, c.second);
        }
    }

    // Pick the most urgent dirty columns (nearest, or soonest on the flight path).
    struct Cand
    {
        int rx, rz, d;
        float eta;
    };
    std::vector<Cand> cands;
    {
        // M5a: columns whose W3 obstacle data changed need a resend too.
        std::vector<std::pair<int, int>> changed;
        collision::TakeChanged(changed);
        std::lock_guard lk(g_mutex);
        for (const auto& c : changed)
            g_dirty.insert(Key(c.first, c.second));
        for (uint64_t k : g_dirty)
        {
            const int rx = KeyA(k), rz = KeyB(k);
            const int d = std::max(std::abs(rx - prx), std::abs(rz - prz));
            float eta = d <= kColumnRadius ? static_cast<float>(std::max(0, d - 1) * kRegion / slow) : INFINITY;
            auto ci = corridor.find(k);
            if (ci != corridor.end())
                eta = std::min(eta, ci->second);
            if (d <= kColumnRadius || ci != corridor.end())
                cands.push_back({rx, rz, d, eta});
        }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.eta != b.eta ? a.eta < b.eta : a.d < b.d; });

    LARGE_INTEGER qf, q0;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&q0);
    int done = 0;
    for (const auto& c : cands)
    {
        if (done >= kColumnsPerIteration)
            break;
        if (done > 0)
        {
            LARGE_INTEGER q;
            QueryPerformanceCounter(&q);
            if (static_cast<double>(q.QuadPart - q0.QuadPart) * 1000.0 / static_cast<double>(qf.QuadPart) > kSendBudgetMs)
                break;
        }
        float h[kRegion + 1][kRegion + 1];
        bool complete = true;
        {
            std::lock_guard lk(g_mutex);
            for (int i = 0; i <= kRegion && complete; ++i)
                for (int k = 0; k <= kRegion; ++k)
                {
                    auto it = g_heights.find(Key(c.rx * kRegion + i, c.rz * kRegion + k));
                    if (it == g_heights.end())
                    {
                        complete = false;
                        break;
                    }
                    h[i][k] = it->second;
                }
        }
        collision::ColumnInfo obs;
        const bool haveObs = collision::GetColumn(c.rx, c.rz, obs);
        if (!complete)
        {
            // M6b: soon on the flight path and W3's collision is scanned: send it now with the
            // corners known so far (missing ones = no height-field cell); the rest follows.
            if (!(c.eta <= kPartialEtaS && haveObs && collision::Active() && obs.ryHi >= obs.ryLo && collision::GetFlightConfig().lookaheadMs > 0))
                continue; // wait until the script has sampled every corner
            std::lock_guard lk(g_mutex);
            for (int i = 0; i <= kRegion; ++i)
                for (int k = 0; k <= kRegion; ++k)
                {
                    auto it = g_heights.find(Key(c.rx * kRegion + i, c.rz * kRegion + k));
                    h[i][k] = it == g_heights.end() ? NAN : it->second;
                }
        }
        auto& state = g_sent[Key(c.rx, c.rz)];
        const uint64_t obsVersion = haveObs ? obs.version : 0;
        const uint64_t hash = HashCorners(h) ^ (obsVersion * 0x9E3779B97F4A7C15ull) ^ (complete ? 0 : 0x5bd1e995ull);
        if (state.hash == hash && state.ryHi >= state.ryLo)
        {
            std::lock_guard lk(g_mutex);
            g_dirty.erase(Key(c.rx, c.rz));
            continue;
        }
        LARGE_INTEGER qc0, qc1;
        QueryPerformanceCounter(&qc0);
        if (!SendColumn(c.rx, c.rz, h, epoch, state, counters, haveObs ? &obs : nullptr))
        {
            counters.ringFull++;
            break; // MC is behind; try again next iteration
        }
        QueryPerformanceCounter(&qc1);
        {
            const double colMs = static_cast<double>(qc1.QuadPart - qc0.QuadPart) * 1000.0 / static_cast<double>(qf.QuadPart);
            counters.sendCols++;
            counters.sendColMsSum += colMs;
            counters.sendColMsMax = std::max(counters.sendColMsMax, colMs);
        }
        state.hash = hash;
        state.processed = true;
        state.obsVersion = obsVersion;
        if (haveObs)
            state.obsRyLo = obs.ryLo, state.obsRyHi = obs.ryHi;
        state.complete = complete;
        state.ringHead = RingHead();
        if (!complete)
            counters.partialSent++;
        if (c.d > kColumnRadius)
            counters.aheadSent++;
        ++done;
        if (complete)
        {
            std::lock_guard lk(g_mutex);
            g_dirty.erase(Key(c.rx, c.rz));
        }
    }
    LARGE_INTEGER q1;
    QueryPerformanceCounter(&q1);
    if (done > 0)
        counters.sendMsMax = std::max(counters.sendMsMax, static_cast<double>(q1.QuadPart - q0.QuadPart) * 1000.0 / static_cast<double>(qf.QuadPart));
}

// Drops far samples; returns the lowest sampled ground within 32 blocks (NAN if none).
float Prune(double px, double pz)
{
    const int ix = static_cast<int>(std::floor(px)), iz = static_cast<int>(std::floor(pz));
    float minGround = NAN;
    std::lock_guard lk(g_mutex);
    for (auto it = g_heights.begin(); it != g_heights.end();)
    {
        const int dx = std::abs(KeyA(it->first) - ix), dz = std::abs(KeyB(it->first) - iz);
        if (dx > kPruneDistance || dz > kPruneDistance)
        {
            g_water.erase(it->first);
            it = g_heights.erase(it);
            continue;
        }
        if (dx <= 32 && dz <= 32 && !std::isnan(it->second) && (std::isnan(minGround) || it->second < minGround))
            minGround = it->second;
        ++it;
    }
    const int prx = FloorDiv(ix, kRegion), prz = FloorDiv(iz, kRegion);
    const int farLimit = kPruneDistance / kRegion + 2;
    for (auto it = g_dirty.begin(); it != g_dirty.end();)
    {
        if (std::abs(KeyA(*it) - prx) > farLimit || std::abs(KeyB(*it) - prz) > farLimit)
            it = g_dirty.erase(it);
        else
            ++it;
    }
    for (auto it = g_sent.begin(); it != g_sent.end();)
    {
        if (std::abs(KeyA(it->first) - prx) > farLimit || std::abs(KeyB(it->first) - prz) > farLimit)
            it = g_sent.erase(it);
        else
            ++it;
    }
    collision::Prune(px, pz, kColumnRadius + 3);
    return minGround;
}

// Highest sampled ground among the 4 lattice corners around (x, z) (MC coords). False if none known.
bool GroundNear(double x, double z, float& out)
{
    const int ix = static_cast<int>(std::floor(x)), iz = static_cast<int>(std::floor(z));
    bool any = false;
    std::lock_guard lk(g_mutex);
    for (int dx = 0; dx <= 1; ++dx)
        for (int dz = 0; dz <= 1; ++dz)
        {
            auto it = g_heights.find(Key(ix + dx, iz + dz));
            if (it == g_heights.end() || std::isnan(it->second))
                continue;
            out = any ? std::max(out, it->second) : it->second;
            any = true;
        }
    return any;
}

// Ground known on the whole 3x3 lattice around (x, z): a spot a rescued player can stand on.
bool SolidAround(double x, double z)
{
    const int ix = static_cast<int>(std::floor(x)), iz = static_cast<int>(std::floor(z));
    std::lock_guard lk(g_mutex);
    for (int dx = -1; dx <= 2; ++dx)
        for (int dz = -1; dz <= 2; ++dz)
        {
            auto it = g_heights.find(Key(ix + dx, iz + dz));
            if (it == g_heights.end() || std::isnan(it->second))
                return false;
        }
    return true;
}

// The hold gate: is Minecraft's collision around (x, z) (MC coords) in place? Every region column
// within kGateRadius of the target has been sent, there is known ground at the target (+-1 m),
// and Minecraft has consumed the whole collision ring. Host thread only (reads g_sent).
bool CollisionReady(double x, double z, bool needClear, uint64_t sinceMs, const char*& why)
{
    if (needClear)
    {
        why = "collision epoch not cleared yet";
        return false;
    }
    const int prx = FloorDiv(static_cast<int>(std::floor(x)), kRegion), prz = FloorDiv(static_cast<int>(std::floor(z)), kRegion);
    for (int dx = -kGateRadius; dx <= kGateRadius; ++dx)
        for (int dz = -kGateRadius; dz <= kGateRadius; ++dz)
        {
            // Processed = sent, or known to have no ground at all (a void column: nothing to send).
            auto it = g_sent.find(Key(prx + dx, prz + dz));
            if (it == g_sent.end() || !it->second.processed)
            {
                why = "columns around the target not sent yet";
                return false;
            }
            // M5a: with W3 obstacle collision on, the walls around the target must have been
            // scanned since the hold began (W3 streams them in) and that scan sent to Minecraft.
            if (collision::Active())
            {
                collision::ColumnInfo obs;
                if (!collision::GetColumn(prx + dx, prz + dz, obs) || obs.scanMs < sinceMs || obs.version != it->second.obsVersion)
                {
                    why = "W3 walls around the target not scanned/sent yet";
                    return false;
                }
            }
        }
    bool ground = false;
    {
        const int ix = static_cast<int>(std::floor(x)), iz = static_cast<int>(std::floor(z));
        std::lock_guard lk(g_mutex);
        for (int dx = -1; dx <= 2 && !ground; ++dx)
            for (int dz = -1; dz <= 2 && !ground; ++dz)
            {
                auto it = g_heights.find(Key(ix + dx, iz + dz));
                ground = it != g_heights.end() && !std::isnan(it->second);
            }
    }
    if (!ground)
    {
        why = "no ground sampled at the target";
        return false;
    }
    auto* ring = g_base + proto::kOffCollisionRing;
    const uint64_t head = A(*reinterpret_cast<uint64_t*>(ring + proto::kColRingHeadOff)).load(std::memory_order_acquire);
    const uint64_t tail = A(*reinterpret_cast<uint64_t*>(ring + proto::kColRingTailOff)).load(std::memory_order_acquire);
    if (tail != head)
    {
        why = "Minecraft hasn't consumed the collision ring yet";
        return false;
    }
    return true;
}

// M6b flight guard: is Minecraft's collision in place everywhere the player goes within horizonS?
// Every column the body touches is sent with the current W3 scan, that scan covers the body's
// height, the terrain is there (W3's height field, or every script corner), and Minecraft has
// consumed the ring up to it. Host thread only.
bool PathReady(const collision::Motion& m, float horizonS, const char*& why, int& badRx, int& badRz)
{
    const uint64_t tail = RingTail();
    const double dt = std::max(0.02, std::min(0.05, 1.0 / std::max(m.HSpeed(), 1.0)));
    for (double t = 0.0;; t += dt)
    {
        const double tt = std::min(t, static_cast<double>(horizonS));
        const double x = m.x + m.vx * tt, y = m.y + m.vy * tt, z = m.z + m.vz * tt;
        const int ryA = FloorDiv(static_cast<int>(std::floor(y - 1.5)), kRegion), ryB = FloorDiv(static_cast<int>(std::floor(y + 2.5)), kRegion);
        for (int cx = FloorDiv(static_cast<int>(std::floor(x - 1.0)), kRegion); cx <= FloorDiv(static_cast<int>(std::floor(x + 1.0)), kRegion); ++cx)
            for (int cz = FloorDiv(static_cast<int>(std::floor(z - 1.0)), kRegion); cz <= FloorDiv(static_cast<int>(std::floor(z + 1.0)), kRegion); ++cz)
            {
                badRx = cx, badRz = cz;
                auto it = g_sent.find(Key(cx, cz));
                if (it == g_sent.end() || !it->second.processed)
                {
                    why = "column not sent";
                    return false;
                }
                bool terrain = it->second.complete;
                if (collision::Active())
                {
                    // Any W3 scan already in Minecraft that covers the body's height will do. A newer
                    // rescan of the same column (every CollisionRefreshMs while walking) only refreshes
                    // walls that are already there; waiting for it held Steve on every jump.
                    collision::ColumnInfo obs;
                    const auto& s = it->second;
                    if (s.obsVersion == 0 || !collision::GetColumn(cx, cz, obs))
                    {
                        why = "W3 scan not sent yet";
                        return false;
                    }
                    if (ryA < s.obsRyLo || ryB > s.obsRyHi)
                    {
                        why = obs.version != s.obsVersion ? "W3 scan not sent yet" : "W3 scan does not cover this height yet";
                        return false;
                    }
                    terrain = terrain || obs.terrainTris > 0;
                }
                if (!terrain)
                {
                    // No terrain in Minecraft for this column yet. That only matters if it could be
                    // at the body's height: the script samples known there (any corner) all lie
                    // well below it = the ground is far below (flying over it), not a hole.
                    int known = 0;
                    float top = -INFINITY;
                    {
                        std::lock_guard lk(g_mutex);
                        for (int i = 0; i <= kRegion; ++i)
                            for (int k = 0; k <= kRegion; ++k)
                            {
                                auto hit = g_heights.find(Key(cx * kRegion + i, cz * kRegion + k));
                                if (hit == g_heights.end())
                                    continue;
                                ++known;
                                if (!std::isnan(hit->second))
                                    top = std::max(top, hit->second);
                            }
                    }
                    if (known == 0 || top > y - 4.0)
                    {
                        why = "no terrain yet (W3 height field or script samples)";
                        return false;
                    }
                }
                if (tail < it->second.ringHead)
                {
                    why = "Minecraft hasn't consumed it yet";
                    return false;
                }
            }
        if (tt >= horizonS)
            break;
    }
    return true;
}

std::atomic<bool> g_haveCentre{false};
std::atomic<double> g_centreX{0}, g_centreY{0}, g_centreZ{0}; // MC coords
// M6b: where the native scanner centres (MC feet; the sampling centre above leads it in flight).
std::atomic<double> g_scanX{0}, g_scanY{0}, g_scanZ{0};

void HostLoop()
{
    WLOG("host: thread started");
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer)
        timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

    // Starts somewhere new each run (like SkyCraft), so an MC still acknowledging the last
    // run's teleport can't be taken for having arrived at this run's.
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    uint32_t teleportSeq = static_cast<uint32_t>(q.QuadPart) | 1u;
    uint32_t epoch = 1;
    bool teleportPending = true;
    bool needClear = true;
    bool mcWasAlive = false;
    uint32_t lastMcPid = 0;
    uint64_t seenWorldGen = 0;
    double lastTpX = 0, lastTpY = 0, lastTpZ = 0;
    double tpX = 0, tpY = 0, tpZ = 0; // the last teleport target we published
    uint64_t lastTpMs = 0;
    // M3a: hold gate, safety net, W3-jump detection
    bool gateWaiting = false;
    uint64_t gateSinceMs = 0, lastGateLogMs = 0;
    bool rescuePending = false;
    double rescueX = 0, rescueY = 0, rescueZ = 0;
    uint64_t lastRescueMs = 0;
    bool haveSafe = false;
    double safeX = 0, safeY = 0, safeZ = 0;
    float minGround = NAN;
    bool haveLastIn = false;
    double lastInX = 0, lastInY = 0, lastInZ = 0;
    // M6b: flight motion + guard
    collision::Motion motion;
    int guardBad = 0;
    bool brakePending = false;
    double brakeX = 0, brakeY = 0, brakeZ = 0;
    uint64_t lastGuardLogMs = 0;
    uint64_t lastPruneMs = 0, lastLogMs = 0;
    bool wasStale = true;
    Status counters{};
    proto::McState mc{};
    bool haveMc = false;

    while (!g_stop.load())
    {
        const uint64_t now = GetTickCount64();
        Heartbeat();

        // Minecraft (re)connected: fresh collision epoch, resend everything, re-place its player.
        const bool mcAlive = McAlive();
        const uint32_t mcPid = McPid();
        const bool newMcProcess = mcAlive && mcPid != 0 && mcPid != lastMcPid;
        if (mcAlive && (!mcWasAlive || newMcProcess))
        {
            WLOG("host: Minecraft connected (pid %u)", mcPid);
            ++epoch;
            needClear = true;
            teleportPending = true;
        }
        if (!mcAlive && mcWasAlive)
            WLOG("host: Minecraft heartbeat lost");
        mcWasAlive = mcAlive;
        if (mcAlive)
            lastMcPid = mcPid;
        haveMc = mcAlive && ReadMcState(mc);
        // M6b: Minecraft's velocity from its raw 20 Hz physics ticks (prev/cur feet).
        motion.valid = false;
        if (haveMc && (mc.flags & proto::kMcInWorld) && mc.tickMs > 1.0f && mc.teleportAck == teleportSeq)
        {
            const double k = 1000.0 / mc.tickMs;
            motion.vx = (mc.curX - mc.prevX) * k, motion.vy = (mc.curY - mc.prevY) * k, motion.vz = (mc.curZ - mc.prevZ) * k;
            motion.x = mc.x, motion.y = mc.y, motion.z = mc.z;
            motion.ms = now;
            motion.valid = motion.Speed() < 120.0; // larger = a teleport between the two ticks
            if (!motion.valid)
                motion.vx = motion.vy = motion.vz = 0.0;
        }

        Input in;
        {
            std::lock_guard lk(g_mutex);
            in = g_in;
            g_in.teleportRequested = false;
        }
        if (in.worldGen != seenWorldGen)
        {
            seenWorldGen = in.worldGen;
            ++epoch;
            needClear = true;
            teleportPending = true;
            WLOG("host: world changed (id %08X) -> collision epoch %u", in.worldId, epoch);
        }
        if (needClear)
            g_sent.clear();

        const bool stale = !in.haveTick || now - in.lastTickMs > kStaleTickMs;
        if (stale != wasStale)
        {
            WLOG("host: script ticks %s", stale ? "stopped (paused/loading): MC holds its player" : "running");
            wasStale = stale;
        }
        const bool inGame = in.haveTick && (in.flags & kTickInGame);
        const bool loading = stale || (in.flags & kTickLoading);
        const bool mcInWorld = haveMc && (mc.flags & proto::kMcInWorld);
        // M3a: Minecraft mode bookkeeping (input hook, cursor, engaged). Decides Driven().
        control::HostUpdate(haveMc ? &mc : nullptr, mcAlive, inGame && !loading, g_viewportW.load(std::memory_order_relaxed),
                            g_viewportH.load(std::memory_order_relaxed));
        const bool driven = control::Driven();
        const bool mcHolding = haveMc && mc.teleportAck != teleportSeq;
        auto dist = [](double ax, double ay, double az, double bx, double by, double bz) {
            return std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by) + (az - bz) * (az - bz));
        };
        const double inToMc = mcInWorld ? dist(in.x, in.y, in.z, mc.x, mc.y, mc.z) : 1e9;

        if (in.teleportRequested)
            teleportPending = true;
        if (loading || !inGame)
        {
            teleportPending = true; // re-place MC's player when we're back
            rescuePending = false;
            brakePending = false;
        }
        else
        {
            // The Witcher moved its player by itself (fast travel without a loading screen, a quest
            // teleport, a scene): put Minecraft's player there. Our own per-step "Geralt follows
            // Minecraft" teleports never trip this (Geralt lands next to Minecraft's feet).
            if (driven && haveLastIn && !teleportPending && inToMc > kStayBlocks &&
                dist(in.x, in.y, in.z, lastInX, lastInY, lastInZ) > kW3JumpBlocks)
            {
                WLOG("host: The Witcher moved its player %.1f m in one step; resyncing Minecraft there",
                     dist(in.x, in.y, in.z, lastInX, lastInY, lastInZ));
                teleportPending = true;
            }

            // Safety net: Minecraft's player fell through the world (missing or late collision).
            if (mcInWorld && !mcHolding && !teleportPending && !(mc.flags & proto::kMcDead) && now - lastRescueMs > kRescueIntervalMs)
            {
                float g = 0.0f;
                const bool haveGround = GroundNear(mc.x, mc.z, g);
                const bool underGround = haveGround && mc.y < g - kRescueBelowGround;
                const bool inVoid = !haveGround && !std::isnan(minGround) && mc.y < minGround - kRescueBelowMin;
                if (haveGround && (mc.flags & proto::kMcOnGround) && std::fabs(mc.y - g) < 1.5 && SolidAround(mc.x, mc.z))
                {
                    haveSafe = true;
                    safeX = mc.x, safeY = mc.y, safeZ = mc.z;
                }
                if (underGround || inVoid)
                {
                    if (haveSafe && dist(safeX, safeY, safeZ, mc.x, safeY, mc.z) < 24.0)
                        rescueX = safeX, rescueY = safeY + 0.05, rescueZ = safeZ;
                    else if (haveGround)
                        rescueX = mc.x, rescueY = g + 0.5, rescueZ = mc.z;
                    else
                        rescueX = in.x, rescueY = in.y + 0.05, rescueZ = in.z;
                    WLOG("host: SAFETY NET: Minecraft's player is at (%.1f, %.1f, %.1f), %s; teleporting it back to (%.1f, %.1f, %.1f)", mc.x,
                         mc.y, mc.z,
                         underGround ? "more than 4 m under the sampled ground" : "more than 10 m under the lowest sampled ground", rescueX,
                         rescueY, rescueZ);
                    rescuePending = true;
                    teleportPending = true;
                    lastRescueMs = now;
                    counters.rescues++;
                }
            }
        }

        // M6b FLIGHT GUARD: Minecraft's player is about to enter space whose collision isn't in
        // Minecraft yet (elytra/rocket speed outran the prefetch): hold it where it is through the
        // hold gate until it is. Only within guardMaxAbove of known ground: Minecraft's own hold
        // lasts 6 s when there is no ground within 12 blocks under the player.
        const collision::FlightConfig fcfg = collision::GetFlightConfig();
        const bool driving = driven && mcInWorld && !mcHolding && !teleportPending && !loading && inGame && motion.valid;
        if (driving && fcfg.lookaheadMs > 0 && fcfg.guardMs > 0 && motion.Speed() >= fcfg.guardMinSpeed)
        {
            const char* gwhy = "";
            int brx = 0, brz = 0;
            const float gh = static_cast<float>(fcfg.guardMs) / 1000.0f;
            if (PathReady(motion, gh, gwhy, brx, brz))
                guardBad = 0;
            else if (++guardBad >= 2)
            {
                // Ground under the player now or where the path ends.
                float g0 = NAN, g1 = NAN, g = 0.0f;
                if (GroundNear(motion.x, motion.z, g))
                    g0 = g;
                if (GroundNear(motion.x + motion.vx * gh, motion.z + motion.vz * gh, g))
                    g1 = g;
                const double yEnd = motion.y + motion.vy * gh;
                const bool groundClose = (!std::isnan(g0) && motion.y - g0 <= fcfg.guardMaxAbove) || (!std::isnan(g1) && yEnd - g1 <= fcfg.guardMaxAbove);
                if (groundClose)
                {
                    counters.flightHolds++;
                    WLOG("host: FLIGHT GUARD: %.1f blocks/s, the path %.0f ms ahead is not ready (column %d,%d: %s); holding Minecraft's player "
                         "at (%.1f, %.1f, %.1f)",
                         motion.Speed(), gh * 1000.0f, brx, brz, gwhy, mc.x, mc.y, mc.z);
                    brakePending = true;
                    brakeX = mc.x, brakeY = mc.y, brakeZ = mc.z;
                    teleportPending = true;
                    guardBad = 0;
                }
                else
                {
                    counters.guardSkips++;
                    if (now - lastGuardLogMs > 2000)
                    {
                        lastGuardLogMs = now;
                        WLOG("host: flight guard: path not ready (column %d,%d: %s) but no known ground within %.0f blocks below; not holding "
                             "(%.1f blocks/s)",
                             brx, brz, gwhy, fcfg.guardMaxAbove, motion.Speed());
                    }
                }
            }
        }
        else
            guardBad = 0;

        // Teleport target: a rescue spot; else where Minecraft's player already is (if the W3
        // player is next to it: a resume after a menu shouldn't yank it); else the W3 player.
        double tgtX = in.x, tgtY = in.y, tgtZ = in.z;
        if (rescuePending)
            tgtX = rescueX, tgtY = rescueY, tgtZ = rescueZ;
        else if (brakePending)
        {
            // Minecraft parks its player where it sees kSkyLoading: follow that spot, so the
            // release (a teleport to this target) doesn't snap it back.
            if (mcInWorld && mcHolding)
                brakeX = mc.x, brakeY = mc.y, brakeZ = mc.z;
            tgtX = brakeX, tgtY = brakeY, tgtZ = brakeZ;
        }
        else if (control::McMode() && mcInWorld && inToMc < kStayBlocks && !mcHolding)
            tgtX = mc.x, tgtY = mc.y, tgtZ = mc.z;

        bool gateHolding = false;
        if (!loading && inGame && teleportPending)
        {
            // Hold gate: don't let Minecraft release its player before the ground around the
            // target is in Minecraft (it would fall through its void world).
            const char* why = "";
            if (!gateWaiting)
            {
                gateWaiting = true;
                gateSinceMs = now;
            }
            // A flight-guard brake keeps Steve where he already was: the latest scan being in
            // Minecraft is enough, no need to wait a whole refresh for a scan newer than the hold.
            const bool ready = mcAlive && CollisionReady(tgtX, tgtZ, needClear, brakePending ? 0 : gateSinceMs, why);
            if (ready || now - gateSinceMs > kGateTimeoutMs)
            {
                if (!ready)
                    WLOG("host: hold gate timed out after %llu ms (%s); teleporting anyway", static_cast<unsigned long long>(now - gateSinceMs), why);
                else if (now - gateSinceMs > 100)
                    WLOG("host: hold gate: collision around (%.1f, %.1f, %.1f) ready after %llu ms; releasing the teleport", tgtX, tgtY, tgtZ,
                         static_cast<unsigned long long>(now - gateSinceMs));
                ++teleportSeq;
                teleportPending = false;
                rescuePending = false;
                brakePending = false;
                gateWaiting = false;
                tpX = tgtX, tpY = tgtY, tpZ = tgtZ;
                lastTpX = tgtX, lastTpY = tgtY, lastTpZ = tgtZ;
                lastTpMs = now;
                counters.teleports++;
            }
            else
            {
                gateHolding = true;
                if (now - lastGateLogMs > 2000)
                {
                    lastGateLogMs = now;
                    counters.gateHolds++;
                    WLOG("host: hold gate: Minecraft player held (%s)", why);
                }
            }
        }
        else if (!loading && inGame && !driven && !(in.flags & kTickNoFollow) && now - lastTpMs >= kFollowIntervalMs)
        {
            // Mirror mode (Witcher mode): MC has no input of its own, so its player only moves when
            // we teleport it. A teleport per MC tick while Geralt moves makes it follow.
            const double dx = in.x - lastTpX, dy = in.y - lastTpY, dz = in.z - lastTpZ;
            if (dx * dx + dy * dy + dz * dz > kFollowEps * kFollowEps)
            {
                ++teleportSeq;
                tpX = in.x, tpY = in.y, tpZ = in.z;
                lastTpX = in.x, lastTpY = in.y, lastTpZ = in.z;
                lastTpMs = now;
                counters.teleports++;
            }
        }
        if (!teleportPending)
            gateWaiting = false;
        // Perf: the scanner rescans the gate's columns now, not at their refresh.
        static const bool gateLegacy = std::getenv("WC_GATE_LEGACY") != nullptr; // A/B knob for the offline perf runs
        collision::SetGate(gateHolding && !brakePending && !gateLegacy, tgtX, tgtZ, kGateRadius, gateSinceMs);
        control::SetArrived(mcInWorld && inGame && !loading && !teleportPending && !gateHolding && mc.teleportAck == teleportSeq);
        if (in.haveTick)
        {
            haveLastIn = true;
            lastInX = in.x, lastInY = in.y, lastInZ = in.z;
        }

        // Where collision is streamed (and the script samples): the target of a pending
        // teleport, else Minecraft's feet while it drives, else the W3 player.
        double cX = in.x, cY = in.y, cZ = in.z;
        if (teleportPending)
            cX = tgtX, cY = tgtY, cZ = tgtZ;
        else if (driven && mcInWorld)
            cX = mc.x, cY = mc.y, cZ = mc.z;
        // M6b: Minecraft's motion for the scanner / streaming / prune (valid only while it drives).
        {
            collision::Motion pub = motion;
            pub.valid = motion.valid && driven && !teleportPending;
            collision::SetMotion(pub);
            // The player's ender pearl in flight: a second scan/stream focus.
            collision::Motion pearl;
            pearl.valid = mcInWorld && driven && (mc.pearlFlags & proto::kPearlInFlight);
            // Perf: how long Minecraft holds a pearl in the air.
            {
                static bool wasFlying = false, wasHeld = false;
                static uint64_t heldSinceMs = 0;
                const bool held = pearl.valid && (mc.pearlFlags & proto::kPearlHeld);
                if (pearl.valid && !wasFlying)
                    counters.pearls++;
                if (held && !wasHeld)
                {
                    heldSinceMs = now;
                    counters.pearlHolds++;
                }
                if (!held && wasHeld)
                {
                    const double ms = static_cast<double>(now - heldSinceMs);
                    counters.pearlHeldMsSum += ms;
                    counters.pearlHeldMsMax = std::max(counters.pearlHeldMsMax, ms);
                    WLOG("host: ender pearl released after %.0f ms in the air (holds %llu, longest %.0f ms)", ms,
                         static_cast<unsigned long long>(counters.pearlHolds), counters.pearlHeldMsMax);
                }
                wasFlying = pearl.valid;
                wasHeld = held;
            }
            if (pearl.valid)
            {
                pearl.x = mc.pearlX, pearl.y = mc.pearlY, pearl.z = mc.pearlZ;
                pearl.vx = mc.pearlVX, pearl.vy = mc.pearlVY, pearl.vz = mc.pearlVZ;
                pearl.ms = now;
                static uint64_t lastPearlLogMs = 0;
                if ((mc.pearlFlags & proto::kPearlHeld) && now - lastPearlLogMs > 1000)
                {
                    lastPearlLogMs = now;
                    WLOG("host: ender pearl held by Minecraft at (%.1f, %.1f, %.1f), velocity (%.1f, %.1f, %.1f) b/s, until the Witcher's collision "
                         "there is in",
                         pearl.x, pearl.y, pearl.z, pearl.vx, pearl.vy, pearl.vz);
                }
            }
            collision::SetFocus(pearl);
        }
        // M6b: the script samples ahead of a fast player (up to 12 m along its horizontal velocity,
        // so its feet stay inside the sampled +-16 m square).
        double sX = cX, sZ = cZ;
        if (!teleportPending && driven && motion.valid && collision::GetFlightConfig().lookaheadMs > 0)
        {
            const double hs = motion.HSpeed();
            const double lead = std::min(hs * 0.5, 12.0);
            if (hs > 1.0)
                sX += motion.vx / hs * lead, sZ += motion.vz / hs * lead;
        }
        if (in.haveTick)
        {
            g_centreX.store(sX);
            g_centreY.store(cY);
            g_centreZ.store(sZ);
            g_scanX.store(cX);
            g_scanY.store(cY);
            g_scanZ.store(cZ);
            g_haveCentre.store(true);
        }

        proto::SkyState sky{};
        sky.flags = (inGame ? proto::kSkyInGame : 0u) | ((in.flags & kTickMenuOpen) ? proto::kSkyMenuOpen : 0u) |
                    ((loading || gateHolding) ? proto::kSkyLoading : 0u);
        sky.worldId = in.worldId;
        sky.collisionEpoch = epoch;
        // MC only reads pos when it applies a teleport (or re-places a new player object after a
        // join / respawn): the last target while that teleport is in flight, else the W3 player.
        const bool tpInFlight = mcHolding || (haveMc && now - lastTpMs < 500);
        sky.posX = (tpInFlight || gateHolding) ? (gateHolding ? tgtX : tpX) : in.x;
        sky.posY = (tpInFlight || gateHolding) ? (gateHolding ? tgtY : tpY) : in.y;
        sky.posZ = (tpInFlight || gateHolding) ? (gateHolding ? tgtZ : tpZ) : in.z;
        if (driven)
        {
            control::GetLook(sky.yaw, sky.pitch); // Minecraft mode: the mouse look (control.cpp)
        }
        else
        {
            sky.yaw = in.yaw;
            sky.pitch = in.pitch;
            // Build mode: MC looks from its own eye at the point the W3 camera aims at (M2b).
            buildmode::AimLook(haveMc ? &mc : nullptr, in.x, in.y, in.z, sky.yaw, sky.pitch);
            if (!control::McMode() || !mcInWorld)
                control::SetLook(sky.yaw, sky.pitch); // so a later engage starts from here
        }
        sky.teleportSeq = teleportSeq;
        sky.viewportW = g_viewportW.load(std::memory_order_relaxed); // 0 = no overlay
        sky.viewportH = g_viewportH.load(std::memory_order_relaxed);
        sky.gameHour = 12.0f;
        WriteSkyState(sky);
        // M7: W3's water around Minecraft's player (or Geralt) for swimming / floating / drowning.
        if (mcInWorld)
            WriteWaterGrid(mc.x, mc.z, in.worldId);
        else
            WriteWaterGrid(in.x, in.z, in.worldId);

        counters.renderBytesDrained += DrainRenderRing();
        counters.eventsDrained += DrainEventRing();
        // M4a combat: actor table, batched damage; hurts are queued for the input ring below.
        combat::HostPump(g_base, mcAlive);
        // Build mode input (keys, clicks, wheel, chat commands) -> input ring (M2b).
        buildmode::HostPump(g_base, mcAlive, haveMc ? &mc : nullptr, mcPid, !stale);

        if (mcAlive)
        {
            if (needClear)
            {
                if (WriteCollision(proto::kColClear, &epoch, sizeof(epoch)))
                {
                    needClear = false;
                    std::lock_guard lk(g_mutex);
                    for (auto& kv : g_heights)
                        MarkDirtyCorner(KeyA(kv.first), KeyB(kv.first));
                    WLOG("host: collision cleared, epoch %u (%zu height samples to resend)", epoch, g_heights.size());
                }
            }
            if (!needClear && in.haveTick)
                StreamCollision(cX, cZ, epoch, counters, motion.valid && driven && !teleportPending ? motion : collision::Motion{});
        }

        if (in.haveTick && now - lastPruneMs > 1000)
        {
            lastPruneMs = now;
            minGround = Prune(cX, cZ);
        }

        {
            std::lock_guard lk(g_statusMutex);
            Status& s = g_status;
            const uint64_t ticks = s.ticks + 1;
            s = counters;
            s.ticks = ticks;
            s.mapped = true;
            s.mcAlive = mcAlive;
            s.mcPid = mcPid;
            s.haveMcState = haveMc;
            if (haveMc)
            {
                s.mcFlags = mc.flags;
                s.mcX = mc.x, s.mcY = mc.y, s.mcZ = mc.z;
                s.mcYaw = mc.yaw, s.mcPitch = mc.pitch;
                s.mcFrame = mc.frameCounter;
                s.teleportAck = mc.teleportAck;
            }
            s.teleportSeq = teleportSeq;
            s.epoch = epoch;
            s.skyFlags = sky.flags;
            s.skyX = sky.posX, s.skyY = sky.posY, s.skyZ = sky.posZ;
            s.driven = driven;
            s.gateHolding = gateHolding;
            s.speed = motion.valid ? motion.Speed() : 0.0;
            std::lock_guard lk2(g_mutex);
            s.heightSamples = g_heights.size();
            s.dirtyColumns = g_dirty.size();
        }

        if (now - lastLogMs > 10000)
        {
            lastLogMs = now;
            WLOG("host: %s", StatusLine().c_str());
        }

        if (timer)
        {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(kPeriodMs) * 10000;
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 100);
        }
        else
        {
            Sleep(kPeriodMs);
        }
    }
    if (timer)
        CloseHandle(timer);
    WLOG("host: thread stopped");
}

void W3ToMc(float wx, float wy, float wz, double& x, double& y, double& z)
{
    x = wx;
    y = wz;
    z = -static_cast<double>(wy);
}
} // namespace

bool Start()
{
    std::call_once(g_once, [] {
        if (!CreateMapping())
            return;
        g_startOk = true;
        g_started = true;
        g_thread = new std::thread(HostLoop);
    });
    return g_startOk;
}

bool Started()
{
    return g_started.load();
}

void Stop()
{
    g_stop = true;
    if (g_thread && g_thread->joinable())
        g_thread->join();
    delete g_thread;
    g_thread = nullptr;
    g_started = false;
}

void Tick(float w3x, float w3y, float w3z, float headingDeg, float pitchDeg, uint32_t flags)
{
    Start();
    double x, y, z;
    W3ToMc(w3x, w3y, w3z, x, y, z);
    // W3 heading: degrees, 0 = +Y (north), counter-clockwise (forward = (-sin h, cos h)).
    // MC yaw: degrees, 0 = +Z (south), clockwise (forward = (-sin yaw, cos yaw) in x/z).
    float yaw = std::fmod(180.0f - headingDeg, 360.0f);
    if (yaw < -180.0f)
        yaw += 360.0f;
    else if (yaw >= 180.0f)
        yaw -= 360.0f;
    const float pitch = std::clamp(-pitchDeg, -90.0f, 90.0f); // W3 pitch up+, MC pitch down+
    std::lock_guard lk(g_mutex);
    g_in.haveTick = true;
    g_in.lastTickMs = GetTickCount64();
    g_in.x = x, g_in.y = y, g_in.z = z;
    g_in.yaw = yaw;
    g_in.pitch = pitch;
    g_in.flags = flags;
    if (flags & kTickTeleport)
        g_in.teleportRequested = true;
}

void PushHeight(int wx, int wy, float groundZ)
{
    std::lock_guard lk(g_mutex);
    StoreHeight(wx, -wy, groundZ);
}

int PushHeights(std::string_view text)
{
    // "x,y,h;x,y,h;..." -- tolerant parser (ints, float or 'n' for none).
    int n = 0;
    std::lock_guard lk(g_mutex);
    size_t i = 0;
    while (i < text.size())
    {
        size_t end = text.find(';', i);
        if (end == std::string_view::npos)
            end = text.size();
        std::string item(text.substr(i, end - i));
        i = end + 1;
        int wx = 0, wy = 0;
        char hbuf[32]{};
        if (std::sscanf(item.c_str(), " %d , %d , %31[^;]", &wx, &wy, hbuf) == 3)
        {
            float h = NAN;
            float w = NAN; // M7: "x,y,h,w": W3's water surface over that ground
            if (hbuf[0] != 'n' && hbuf[0] != 'N')
            {
                char* endp = nullptr;
                h = std::strtof(hbuf, &endp);
                if (endp == hbuf)
                    continue;
                if (*endp == ',')
                {
                    char* wend = nullptr;
                    const float wv = std::strtof(endp + 1, &wend);
                    if (wend != endp + 1)
                        w = wv;
                }
            }
            StoreHeight(wx, -wy, h);
            if (std::isnan(w))
                g_water.erase(Key(wx, -wy));
            else
                g_water[Key(wx, -wy)] = w;
            ++n;
        }
    }
    return n;
}

void SetWorld(std::string_view name)
{
    uint32_t id = 2166136261u;
    for (char c : name)
        id = (id ^ static_cast<uint8_t>(c)) * 16777619u;
    std::lock_guard lk(g_mutex);
    if (g_in.worldGen != 0 && g_in.worldId == id)
        return;
    WLOG("host: world \"%.*s\" -> id %08X", static_cast<int>(name.size()), name.data(), id);
    g_in.worldId = id;
    g_in.worldGen++;
    g_heights.clear();
    g_water.clear();
    g_dirty.clear();
    collision::Clear();
}

void SetViewport(uint32_t width, uint32_t height)
{
    width = std::min(width, proto::kMaxOverlayW);
    height = std::min(height, proto::kMaxOverlayH);
    g_viewportW.store(width, std::memory_order_relaxed);
    g_viewportH.store(height, std::memory_order_relaxed);
}

uint8_t* MappingBase()
{
    return g_base;
}

bool GroundAt(int mcx, int mcz, float& mcy)
{
    std::lock_guard lk(g_mutex);
    auto it = g_heights.find(Key(mcx, mcz));
    if (it == g_heights.end())
        return false;
    mcy = it->second;
    return true;
}

bool ReadMc(proto::McState& out)
{
    if (!g_base || !McAlive())
        return false;
    return ReadMcState(out);
}

bool SampleCentre(float& w3x, float& w3y, float& w3z)
{
    if (!g_haveCentre.load())
        return false;
    w3x = static_cast<float>(g_centreX.load());
    w3y = static_cast<float>(-g_centreZ.load());
    w3z = static_cast<float>(g_centreY.load());
    return true;
}

bool ScanCentre(float& w3x, float& w3y, float& w3z)
{
    if (!g_haveCentre.load())
        return false;
    w3x = static_cast<float>(g_scanX.load());
    w3y = static_cast<float>(-g_scanZ.load());
    w3z = static_cast<float>(g_scanY.load());
    return true;
}

Status GetStatus()
{
    std::lock_guard lk(g_statusMutex);
    Status s = g_status;
    s.mapped = g_started.load();
    return s;
}

std::string StatusLine()
{
    const Status s = GetStatus();
    char buf[512];
    if (!s.mapped)
        return "WitcherCraft: host not running (mapping failed?)";
    // MC feet back in W3 coordinates.
    const double wx = s.mcX, wy = -s.mcZ, wz = s.mcY;
    const double gap = std::sqrt((s.mcX - s.skyX) * (s.mcX - s.skyX) + (s.mcY - s.skyY) * (s.mcY - s.skyY) + (s.mcZ - s.skyZ) * (s.mcZ - s.skyZ));
    std::snprintf(buf, sizeof(buf),
                  "MC %s pid=%u world=%s | mc W3pos=(%.1f, %.1f, %.1f) gap=%.2fm %s%s| frame=%llu tp=%s | cols=%llu blocks=%llu tris=%llu "
                  "samples=%zu dirty=%zu full=%llu",
                  s.mcAlive ? "LINKED" : "not linked", s.mcPid, (s.haveMcState && (s.mcFlags & proto::kMcInWorld)) ? "in" : "no", wx, wy, wz,
                  s.haveMcState ? gap : -1.0, (s.mcFlags & proto::kMcOnGround) ? "ground " : "air ",
                  (s.mcFlags & proto::kMcDead) ? "DEAD " : "", static_cast<unsigned long long>(s.mcFrame),
                  s.teleportAck == s.teleportSeq ? "ok" : "holding", static_cast<unsigned long long>(s.columnsSent),
                  static_cast<unsigned long long>(s.blocksSent), static_cast<unsigned long long>(s.trisSent), s.heightSamples,
                  s.dirtyColumns, static_cast<unsigned long long>(s.ringFull));
    char m3[128];
    std::snprintf(m3, sizeof(m3), " | gate=%s holds=%llu rescues=%llu obsTris=%llu groundMatch=%.0f%%", s.gateHolding ? "HOLD" : "open",
                  static_cast<unsigned long long>(s.gateHolds), static_cast<unsigned long long>(s.rescues),
                  static_cast<unsigned long long>(s.obstacleTrisSent),
                  s.obsCorners ? 100.0 * static_cast<double>(s.obsCornersExplained) / static_cast<double>(s.obsCorners) : 0.0);
    char m6[160]; // M6b flight
    std::snprintf(m6, sizeof(m6), " | flight v=%.0f b/s guardHolds=%llu guardSkips=%llu ahead=%llu early=%llu sendMax=%.1fms", s.speed,
                  static_cast<unsigned long long>(s.flightHolds), static_cast<unsigned long long>(s.guardSkips),
                  static_cast<unsigned long long>(s.aheadSent), static_cast<unsigned long long>(s.partialSent), s.sendMsMax);
    char pf[192]; // perf
    std::snprintf(pf, sizeof(pf), " | perf colBuild avg=%.2fms max=%.1fms n=%llu pearls=%llu held=%llu heldMax=%.0fms heldAvg=%.0fms",
                  s.sendCols ? s.sendColMsSum / static_cast<double>(s.sendCols) : 0.0, s.sendColMsMax, static_cast<unsigned long long>(s.sendCols),
                  static_cast<unsigned long long>(s.pearls), static_cast<unsigned long long>(s.pearlHolds), s.pearlHeldMsMax,
                  s.pearlHolds ? s.pearlHeldMsSum / static_cast<double>(s.pearlHolds) : 0.0);
    return std::string(buf) + blocks::StatusSuffix() + buildmode::StatusSuffix() + control::StatusSuffix() + m3 + m6 + pf + combat::StatusSuffix();
}
} // namespace wc::host
