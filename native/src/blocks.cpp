// WitcherCraft M2b: Minecraft blocks in The Witcher 3. See blocks.hpp.
#include "blocks.hpp"

#include "log.hpp"
#include "skycraft_host.hpp"
#include "skycraft_protocol.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wc::blocks
{
namespace proto = skycraft::proto;

namespace
{
constexpr int kSectionBlocks = 16 * 16 * 16;
constexpr uint64_t kRequeueMs = 2000; // re-check buried / capped blocks this often

// ---- keys ---------------------------------------------------------------------------------
// Block key: x, z 24-bit signed, y 16-bit signed (MC coords; W3 maps fit easily).
uint64_t BlockKey(int x, int y, int z)
{
    return CellKey(x, y, z);
}
int SignExtend(uint64_t v, int bits)
{
    const uint64_t m = 1ull << (bits - 1);
    v &= (1ull << bits) - 1;
    return static_cast<int>(static_cast<int64_t>((v ^ m)) - static_cast<int64_t>(m));
}
void UnKey(uint64_t k, int& x, int& y, int& z)
{
    x = SignExtend(k >> 40, 24);
    z = SignExtend(k >> 16, 24);
    y = SignExtend(k, 16);
}
uint64_t SectionKey(int sx, int sy, int sz)
{
    return BlockKey(sx, sy, sz);
}

int FloorDiv16(int v)
{
    return v >= 0 ? v / 16 : -((-v + 15) / 16);
}

// ---- state (all under g_mutex) ------------------------------------------------------------
struct Section
{
    std::array<uint64_t, 64> solid{};   // kRenSolids bits (x + 16z + 256y)
    std::array<uint64_t, 64> face{};    // has at least one face in kRenSection's mesh
    std::array<uint64_t, 64> visible{}; // solid & face, as last published to g_desired
    std::array<uint8_t, kSectionBlocks> cls{};
};

struct Spawned
{
    int id;
    uint8_t cls;
};

std::mutex g_mutex;
std::unordered_map<uint64_t, std::unique_ptr<Section>> g_sections;
std::unordered_map<uint64_t, uint8_t> g_desired;  // visible block -> class
std::unordered_map<uint64_t, Spawned> g_spawned;  // what the script has an entity for
std::unordered_map<uint64_t, uint64_t> g_gone;    // entity removed at (ms): still a stand-in for kStandInGraceMs
std::deque<uint64_t> g_queue;                     // keys to reconcile
std::unordered_set<uint64_t> g_queued;
std::unordered_set<uint64_t> g_overflow; // visible but over the spawn cap
std::unordered_set<uint64_t> g_buried;   // visible but under the W3 ground: not shown
std::vector<int> g_freeIds;
int g_nextId = 0;
uint64_t g_lastRequeueMs = 0;
Stats g_stats;
bool g_capLogged = false;

// Atlas (kRenAtlas): RGBA8, top row first. Host thread only (no lock needed for reads there).
std::vector<uint8_t> g_atlas;
uint32_t g_atlasW = 0, g_atlasH = 0;
std::unordered_map<uint64_t, uint32_t> g_rectColor; // pixel rect -> averaged RGB (host thread only)

void Enqueue(uint64_t key) // caller holds g_mutex
{
    if (g_queued.insert(key).second)
        g_queue.push_back(key);
}

// ---- colour -------------------------------------------------------------------------------
uint32_t AverageRect(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    if (g_atlas.empty() || x1 <= x0 || y1 <= y0)
        return 0x808080;
    const uint64_t key = (static_cast<uint64_t>(x0) << 48) | (static_cast<uint64_t>(y0) << 32) | (static_cast<uint64_t>(x1) << 16) | y1;
    if (auto it = g_rectColor.find(key); it != g_rectColor.end())
        return it->second;
    // Sample at most 32x32 texels of the rect.
    const uint32_t stepX = std::max(1u, (x1 - x0) / 32), stepY = std::max(1u, (y1 - y0) / 32);
    uint64_t r = 0, g = 0, b = 0, n = 0;
    for (uint32_t y = y0; y < y1; y += stepY)
        for (uint32_t x = x0; x < x1; x += stepX)
        {
            const uint8_t* p = &g_atlas[(static_cast<size_t>(y) * g_atlasW + x) * 4];
            if (p[3] < 32)
                continue; // cutout holes (leaves, glass edges)
            r += p[0], g += p[1], b += p[2], ++n;
        }
    const uint32_t rgb = n ? static_cast<uint32_t>(r / n) | (static_cast<uint32_t>(g / n) << 8) | (static_cast<uint32_t>(b / n) << 16) : 0x808080;
    if (g_rectColor.size() > 100000)
        g_rectColor.clear();
    g_rectColor.emplace(key, rgb);
    return rgb;
}

uint8_t Classify(uint32_t rgb)
{
    const float r = (rgb & 0xFF) / 255.0f, g = ((rgb >> 8) & 0xFF) / 255.0f, b = ((rgb >> 16) & 0xFF) / 255.0f;
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    const float lum = 0.299f * r + 0.587f * g + 0.114f * b;
    const float sat = mx > 0.0f ? (mx - mn) / mx : 0.0f;
    if (lum < 0.20f)
        return kDark;
    float hue = 0.0f; // degrees
    if (mx > mn)
    {
        if (mx == r)
            hue = 60.0f * std::fmod((g - b) / (mx - mn), 6.0f);
        else if (mx == g)
            hue = 60.0f * ((b - r) / (mx - mn) + 2.0f);
        else
            hue = 60.0f * ((r - g) / (mx - mn) + 4.0f);
        if (hue < 0.0f)
            hue += 360.0f;
    }
    if (sat < 0.18f)
        return lum > 0.62f ? kLight : (lum < 0.30f ? kDark : kStone);
    if (hue >= 65.0f && hue < 170.0f)
        return kGreen;
    if (hue >= 15.0f && hue < 65.0f)
        return (lum > 0.55f && sat < 0.5f) ? kLight : kWood;
    return kVivid;
}

// ---- messages -----------------------------------------------------------------------------

// Publishes the section's visible set (solid & face) into g_desired. Caller holds g_mutex.
void Republish(uint64_t skey, Section& s, int sx, int sy, int sz)
{
    for (int w = 0; w < 64; ++w)
    {
        const uint64_t now = s.solid[w] & s.face[w];
        const uint64_t was = s.visible[w];
        uint64_t touched = now | was;
        while (touched)
        {
            const int bit = std::countr_zero(touched);
            touched &= touched - 1;
            const int idx = w * 64 + bit;
            const int lx = idx & 15, lz = (idx >> 4) & 15, ly = idx >> 8;
            const uint64_t key = BlockKey(sx * 16 + lx, sy * 16 + ly, sz * 16 + lz);
            const bool vis = (now >> bit) & 1;
            if (vis)
            {
                auto [it, inserted] = g_desired.try_emplace(key, s.cls[idx]);
                if (inserted || it->second != s.cls[idx])
                {
                    it->second = s.cls[idx];
                    Enqueue(key);
                }
            }
            else if (g_desired.erase(key))
            {
                Enqueue(key);
            }
        }
        s.visible[w] = now;
    }
    const bool empty = std::all_of(s.solid.begin(), s.solid.end(), [](uint64_t v) { return v == 0; }) &&
                       std::all_of(s.face.begin(), s.face.end(), [](uint64_t v) { return v == 0; });
    if (empty)
        g_sections.erase(skey);
}

void OnSection(const uint8_t* p, uint32_t bytes)
{
    if (bytes < sizeof(proto::RenSection))
        return;
    proto::RenSection hdr;
    std::memcpy(&hdr, p, sizeof(hdr));
    const uint64_t vertsAvail = (bytes - sizeof(hdr)) / sizeof(proto::RenVertex);
    const uint32_t count = static_cast<uint32_t>(std::min<uint64_t>(hdr.vertexCount, vertsAvail)) / 3 * 3;
    const auto* v = reinterpret_cast<const proto::RenVertex*>(p + sizeof(hdr));

    // Per block: the best face (top > side > bottom > unshaded) and its atlas rect + tint.
    struct Face
    {
        uint8_t prio;
        uint16_t x0, y0, x1, y1;
        uint8_t tr, tg, tb;
    };
    static std::array<Face, kSectionBlocks> faces; // host thread only
    std::memset(faces.data(), 0, sizeof(faces));
    std::array<uint64_t, 64> faceBits{};
    static const float kNormals[7][3] = {{0, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
    static const uint8_t kPrio[7] = {1, 2, 4, 3, 3, 3, 3};
    for (uint32_t i = 0; i < count; i += 3)
    {
        const auto& a = v[i];
        const auto& b = v[i + 1];
        const auto& c = v[i + 2];
        if (a.flags & 2u)
            continue; // translucent (water, stained glass panes...): not a block face we show
        const uint32_t n = (a.flags >> 4) & 7u;
        const float* nv = kNormals[n <= 6 ? n : 0];
        const float cx = (a.x + b.x + c.x) / 3.0f - nv[0] * 0.25f;
        const float cy = (a.y + b.y + c.y) / 3.0f - nv[1] * 0.25f;
        const float cz = (a.z + b.z + c.z) / 3.0f - nv[2] * 0.25f;
        const int lx = static_cast<int>(std::floor(cx)), ly = static_cast<int>(std::floor(cy)), lz = static_cast<int>(std::floor(cz));
        if (lx < 0 || lx > 15 || ly < 0 || ly > 15 || lz < 0 || lz > 15)
            continue;
        const int idx = lx + 16 * lz + 256 * ly;
        faceBits[idx >> 6] |= 1ull << (idx & 63);
        const uint8_t prio = kPrio[n <= 6 ? n : 0];
        Face& f = faces[idx];
        if (prio <= f.prio)
            continue;
        f.prio = prio;
        const float u0 = std::min({a.u, b.u, c.u}), u1 = std::max({a.u, b.u, c.u});
        const float v0 = std::min({a.v, b.v, c.v}), v1 = std::max({a.v, b.v, c.v});
        const float W = static_cast<float>(g_atlasW), H = static_cast<float>(g_atlasH);
        f.x0 = static_cast<uint16_t>(std::clamp(std::floor(u0 * W + 0.01f), 0.0f, W));
        f.x1 = static_cast<uint16_t>(std::clamp(std::ceil(u1 * W - 0.01f), 0.0f, W));
        f.y0 = static_cast<uint16_t>(std::clamp(std::floor(v0 * H + 0.01f), 0.0f, H));
        f.y1 = static_cast<uint16_t>(std::clamp(std::ceil(v1 * H - 0.01f), 0.0f, H));
        // Vertex colour = biome tint * ambient occlusion: the brightest corner is closest to the tint.
        auto ch = [&](int shift) {
            return static_cast<uint8_t>(std::max({(a.color >> shift) & 0xFFu, (b.color >> shift) & 0xFFu, (c.color >> shift) & 0xFFu}));
        };
        f.tr = ch(0), f.tg = ch(8), f.tb = ch(16);
    }
    std::array<uint8_t, kSectionBlocks> cls{};
    for (int w = 0; w < 64; ++w)
    {
        uint64_t bitsLeft = faceBits[w];
        while (bitsLeft)
        {
            const int idx = w * 64 + std::countr_zero(bitsLeft);
            bitsLeft &= bitsLeft - 1;
            const Face& f = faces[idx];
            const uint32_t base = AverageRect(f.x0, f.y0, f.x1, f.y1);
            const uint32_t r = (base & 0xFF) * f.tr / 255, g = ((base >> 8) & 0xFF) * f.tg / 255, b = ((base >> 16) & 0xFF) * f.tb / 255;
            cls[idx] = Classify(r | (g << 8) | (b << 16));
        }
    }

    const uint64_t skey = SectionKey(hdr.sx, hdr.sy, hdr.sz);
    std::lock_guard lk(g_mutex);
    g_stats.sectionMsgs++;
    auto& slot = g_sections[skey];
    if (!slot)
        slot = std::make_unique<Section>();
    slot->face = faceBits;
    slot->cls = cls;
    Republish(skey, *slot, hdr.sx, hdr.sy, hdr.sz);
}

void OnSolids(const uint8_t* p, uint32_t bytes)
{
    if (bytes < sizeof(proto::RenSolids))
        return;
    proto::RenSolids hdr;
    std::memcpy(&hdr, p, sizeof(hdr));
    std::array<uint64_t, 64> bits{};
    if (hdr.count > 0 && bytes >= sizeof(hdr) + 512)
        std::memcpy(bits.data(), p + sizeof(hdr), 512);
    const uint64_t skey = SectionKey(hdr.sx, hdr.sy, hdr.sz);
    std::lock_guard lk(g_mutex);
    g_stats.solidMsgs++;
    auto& slot = g_sections[skey];
    if (!slot)
        slot = std::make_unique<Section>();
    slot->solid = bits;
    Republish(skey, *slot, hdr.sx, hdr.sy, hdr.sz);
}

void OnAtlas(const uint8_t* p, uint32_t bytes)
{
    if (bytes < sizeof(proto::RenAtlas))
        return;
    proto::RenAtlas hdr;
    std::memcpy(&hdr, p, sizeof(hdr));
    const uint64_t need = static_cast<uint64_t>(hdr.width) * hdr.height * 4;
    if (hdr.width == 0 || hdr.height == 0 || hdr.width > 16384 || hdr.height > 16384 || bytes < sizeof(hdr) + need)
    {
        WLOG("blocks: bad atlas message (%ux%u, %u bytes)", hdr.width, hdr.height, bytes);
        return;
    }
    g_atlas.assign(p + sizeof(hdr), p + sizeof(hdr) + need);
    g_atlasW = hdr.width;
    g_atlasH = hdr.height;
    g_rectColor.clear();
    std::lock_guard lk(g_mutex);
    g_stats.atlasMsgs++;
    g_stats.atlasW = hdr.width;
    g_stats.atlasH = hdr.height;
    WLOG("blocks: atlas %ux%u received", hdr.width, hdr.height);
}

void ClearAll() // caller holds g_mutex
{
    g_sections.clear();
    for (auto& kv : g_desired)
        Enqueue(kv.first);
    g_desired.clear();
    g_overflow.clear();
    g_buried.clear();
    g_stats.clears++;
}

// Is the block's top more than a block below the W3 ground the script sampled (a filler block)?
bool Buried(int bx, int by, int bz)
{
    float h[4];
    if (!host::GroundAt(bx, bz, h[0]) || !host::GroundAt(bx + 1, bz, h[1]) || !host::GroundAt(bx, bz + 1, h[2]) ||
        !host::GroundAt(bx + 1, bz + 1, h[3]))
        return false; // ground not known there: show it
    for (float v : h)
        if (std::isnan(v))
            return false;
    const float ground = std::min({h[0], h[1], h[2], h[3]});
    return static_cast<float>(by + 1) < ground - 1.0f;
}

int AllocId()
{
    if (!g_freeIds.empty())
    {
        const int id = g_freeIds.back();
        g_freeIds.pop_back();
        return id;
    }
    return g_nextId++;
}
} // namespace

void OnRenderMessage(uint32_t type, const uint8_t* payload, uint32_t bytes)
{
    switch (type)
    {
    case proto::kRenSection:
        OnSection(payload, bytes);
        break;
    case proto::kRenSolids:
        OnSolids(payload, bytes);
        break;
    case proto::kRenAtlas:
        OnAtlas(payload, bytes);
        break;
    case proto::kRenClearAll: {
        std::lock_guard lk(g_mutex);
        ClearAll();
        WLOG("blocks: Minecraft cleared all sections (%zu spawned blocks to re-check)", g_spawned.size());
        break;
    }
    default:
        break; // lights, dug cells, avatar, scene, textures, animated sprites: not used here
    }
}

void OnMcReconnect()
{
    std::lock_guard lk(g_mutex);
    ClearAll();
}

std::string Poll(int maxItems)
{
    std::string out;
    if (maxItems <= 0)
        return out;
    const uint64_t now = GetTickCount64();
    std::lock_guard lk(g_mutex);
    if (now - g_lastRequeueMs > kRequeueMs)
    {
        g_lastRequeueMs = now;
        for (uint64_t k : g_buried)
            Enqueue(k);
        g_buried.clear();
        if (static_cast<int>(g_spawned.size()) < kMaxSpawned)
        {
            for (uint64_t k : g_overflow)
                Enqueue(k);
            g_overflow.clear();
        }
        // StandInCells prunes too, but only the PhysX scan calls it (off with Collision=0 or after
        // faults): without this the map would grow by one entry per broken block all session.
        for (auto it = g_gone.begin(); it != g_gone.end();)
        {
            if (now - it->second > kStandInGraceMs)
                it = g_gone.erase(it);
            else
                ++it;
        }
    }
    int items = 0;
    char buf[96];
    while (items < maxItems && !g_queue.empty())
    {
        const uint64_t key = g_queue.front();
        g_queue.pop_front();
        g_queued.erase(key);
        const auto d = g_desired.find(key);
        auto s = g_spawned.find(key);
        if (s != g_spawned.end() && (d == g_desired.end() || d->second != s->second.cls))
        {
            std::snprintf(buf, sizeof(buf), "-%d;", s->second.id);
            out += buf;
            ++items;
            g_stats.removes++;
            g_gone[key] = now;
            g_freeIds.push_back(s->second.id);
            g_spawned.erase(s);
            s = g_spawned.end();
        }
        if (d == g_desired.end() || s != g_spawned.end())
            continue;
        int bx, by, bz;
        UnKey(key, bx, by, bz);
        if (static_cast<int>(g_spawned.size()) >= kMaxSpawned)
        {
            g_overflow.insert(key);
            if (!g_capLogged)
            {
                g_capLogged = true;
                g_stats.capHit = true;
                WLOG("blocks: spawn cap %d reached; further blocks wait until others go", kMaxSpawned);
            }
            continue;
        }
        if (Buried(bx, by, bz))
        {
            g_buried.insert(key);
            continue;
        }
        if (items >= maxItems)
        {
            Enqueue(key); // no room for this add in this batch
            break;
        }
        const int id = AllocId();
        g_spawned.emplace(key, Spawned{id, d->second});
        int wx, wy, wz;
        McBlockToW3(bx, by, bz, wx, wy, wz);
        std::snprintf(buf, sizeof(buf), "+%d,%d,%d,%d,%u;", id, wx, wy, wz, d->second);
        out += buf;
        ++items;
        g_stats.adds++;
    }
    return out;
}

void ResetSpawned()
{
    std::lock_guard lk(g_mutex);
    const uint64_t now = GetTickCount64();
    for (const auto& kv : g_spawned)
        g_gone[kv.first] = now;
    g_spawned.clear();
    g_freeIds.clear();
    g_nextId = 0;
    g_queue.clear();
    g_queued.clear();
    g_overflow.clear();
    g_buried.clear();
    g_capLogged = false;
    for (auto& kv : g_desired)
        Enqueue(kv.first);
    WLOG("blocks: spawn bookkeeping reset (%zu visible blocks to announce)", g_desired.size());
}

void StandInCells(std::vector<uint64_t>& sortedKeys)
{
    sortedKeys.clear();
    const uint64_t now = GetTickCount64();
    std::lock_guard lk(g_mutex);
    for (auto it = g_gone.begin(); it != g_gone.end();)
    {
        if (now - it->second > kStandInGraceMs)
            it = g_gone.erase(it);
        else
            sortedKeys.push_back((it++)->first);
    }
    for (const auto& kv : g_spawned)
        sortedKeys.push_back(kv.first);
    std::sort(sortedKeys.begin(), sortedKeys.end());
}

bool Raycast(const double origin[3], const double dir[3], double maxT, double& tHit)
{
    // Amanatides-Woo voxel walk over the visible block set.
    int x = static_cast<int>(std::floor(origin[0])), y = static_cast<int>(std::floor(origin[1])), z = static_cast<int>(std::floor(origin[2]));
    const int stepX = dir[0] > 0 ? 1 : -1, stepY = dir[1] > 0 ? 1 : -1, stepZ = dir[2] > 0 ? 1 : -1;
    const double inf = 1e30;
    const double dX = dir[0] == 0 ? inf : std::fabs(1.0 / dir[0]);
    const double dY = dir[1] == 0 ? inf : std::fabs(1.0 / dir[1]);
    const double dZ = dir[2] == 0 ? inf : std::fabs(1.0 / dir[2]);
    double tX = dir[0] == 0 ? inf : (dir[0] > 0 ? x + 1 - origin[0] : origin[0] - x) * dX;
    double tY = dir[1] == 0 ? inf : (dir[1] > 0 ? y + 1 - origin[1] : origin[1] - y) * dY;
    double tZ = dir[2] == 0 ? inf : (dir[2] > 0 ? z + 1 - origin[2] : origin[2] - z) * dZ;
    std::lock_guard lk(g_mutex);
    if (g_desired.empty())
        return false;
    if (g_desired.count(BlockKey(x, y, z)))
    {
        tHit = 0.0;
        return true;
    }
    for (int i = 0; i < 256; ++i)
    {
        double t;
        if (tX <= tY && tX <= tZ)
        {
            t = tX;
            tX += dX;
            x += stepX;
        }
        else if (tY <= tZ)
        {
            t = tY;
            tY += dY;
            y += stepY;
        }
        else
        {
            t = tZ;
            tZ += dZ;
            z += stepZ;
        }
        if (t > maxT)
            return false;
        if (g_desired.count(BlockKey(x, y, z)))
        {
            tHit = t;
            return true;
        }
    }
    return false;
}

Stats GetStats()
{
    std::lock_guard lk(g_mutex);
    Stats s = g_stats;
    s.sections = g_sections.size();
    s.visible = g_desired.size();
    s.spawned = g_spawned.size();
    s.pending = g_queue.size();
    s.overflow = g_overflow.size();
    s.buried = g_buried.size();
    return s;
}

std::string StatusSuffix()
{
    const Stats s = GetStats();
    char buf[200];
    std::snprintf(buf, sizeof(buf), " | blocks vis=%zu spawned=%zu pending=%zu%s buried=%zu sect=%zu atlas=%ux%u", s.visible, s.spawned, s.pending,
                  s.overflow ? " CAP" : "", s.buried, s.sections, s.atlasW, s.atlasH);
    return buf;
}
} // namespace wc::blocks
