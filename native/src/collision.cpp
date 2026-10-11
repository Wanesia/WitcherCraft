// M5a obstacle store: W3 collision triangles per region column (MC coords). See collision.hpp.
#include "collision.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <Windows.h>

namespace wc::collision
{
namespace
{
constexpr int kRegion = 8;

int FloorDiv(int v, int d)
{
    return v >= 0 ? v / d : -((-v + d - 1) / d);
}
uint64_t ColKey(int rx, int rz)
{
    return (static_cast<uint64_t>(static_cast<uint32_t>(rx)) << 32) | static_cast<uint32_t>(rz);
}
int KeyX(uint64_t k)
{
    return static_cast<int32_t>(k >> 32);
}
int KeyZ(uint64_t k)
{
    return static_cast<int32_t>(k & 0xFFFFFFFFu);
}

struct Column
{
    uint64_t version = 0;
    uint64_t scanMs = 0;
    uint64_t hash = 0;
    int ryLo = 0, ryHi = -1;
    std::unordered_map<int, std::vector<proto::ColTri>> regions; // ry -> tris
    size_t tris = 0;
    size_t terrainTris = 0;
};

std::mutex g_mutex;
std::unordered_map<uint64_t, Column> g_cols;
std::unordered_set<uint64_t> g_changed;
uint64_t g_versionCounter = 0;
std::atomic<bool> g_active{false};
std::atomic<uint64_t> g_sets{0}, g_changes{0};

uint64_t HashTris(int ryLo, int ryHi, const proto::ColTri* t, size_t n)
{
    uint64_t h = 1469598103934665603ull ^ (static_cast<uint64_t>(static_cast<uint32_t>(ryLo)) << 20) ^ static_cast<uint32_t>(ryHi);
    auto mix = [&](uint32_t v) { h = (h ^ v) * 1099511628211ull; };
    for (size_t i = 0; i < n; ++i)
    {
        for (float f : t[i].v)
            mix(static_cast<uint32_t>(static_cast<int32_t>(std::lround(f * 64.0f)))); // 1/64 block
        mix(t[i].flags);
    }
    mix(static_cast<uint32_t>(n));
    return h;
}
} // namespace

void SetColumn(int rx, int rz, int ryLo, int ryHi, const proto::ColTri* tris, size_t count, uint64_t nowMs)
{
    const uint64_t hash = HashTris(ryLo, ryHi, tris, count);
    g_sets.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lk(g_mutex);
    Column& c = g_cols[ColKey(rx, rz)];
    c.scanMs = nowMs;
    if (c.version != 0 && c.hash == hash)
        return;
    c.hash = hash;
    c.version = ++g_versionCounter;
    c.ryLo = ryLo;
    c.ryHi = ryHi;
    c.regions.clear();
    c.tris = count;
    c.terrainTris = 0;
    for (size_t i = 0; i < count; ++i)
    {
        const proto::ColTri& t = tris[i];
        if (t.flags & proto::kTriTerrain)
            ++c.terrainTris;
        const float lo = std::min({t.v[1], t.v[4], t.v[7]}), hi = std::max({t.v[1], t.v[4], t.v[7]});
        int a = FloorDiv(static_cast<int>(std::floor(lo)), kRegion), b = FloorDiv(static_cast<int>(std::floor(hi)), kRegion);
        a = std::max(a, ryLo);
        b = std::min(b, ryHi);
        for (int ry = a; ry <= b; ++ry)
            c.regions[ry].push_back(t);
    }
    g_changed.insert(ColKey(rx, rz));
    g_changes.fetch_add(1, std::memory_order_relaxed);
}

void ClearPlan();

void Clear()
{
    {
        std::lock_guard lk(g_mutex);
        for (auto& kv : g_cols)
            g_changed.insert(kv.first);
        g_cols.clear();
    }
    ClearPlan(); // a new world: every column is unscanned again
}

void Prune(double cx, double cz, int keep)
{
    const int prx = FloorDiv(static_cast<int>(std::floor(cx)), kRegion), prz = FloorDiv(static_cast<int>(std::floor(cz)), kRegion);
    // M6b: columns on the predicted flight path stay (they were prefetched for a reason).
    std::unordered_set<uint64_t> path;
    Motion m;
    if (GetMotion(m, GetTickCount64()))
    {
        std::vector<std::pair<uint64_t, float>> cor;
        CorridorColumns(m, Horizon(m), 16.0f, cor);
        for (auto& c : cor)
            path.insert(c.first);
    }
    std::lock_guard lk(g_mutex);
    for (auto it = g_cols.begin(); it != g_cols.end();)
    {
        if ((std::abs(KeyX(it->first) - prx) > keep || std::abs(KeyZ(it->first) - prz) > keep) && !path.count(it->first))
            it = g_cols.erase(it);
        else
            ++it;
    }
}

bool GetColumn(int rx, int rz, ColumnInfo& out)
{
    std::lock_guard lk(g_mutex);
    auto it = g_cols.find(ColKey(rx, rz));
    if (it == g_cols.end())
        return false;
    out.version = it->second.version;
    out.scanMs = it->second.scanMs;
    out.ryLo = it->second.ryLo;
    out.ryHi = it->second.ryHi;
    out.tris = it->second.tris;
    out.terrainTris = it->second.terrainTris;
    return true;
}

void AppendRegion(int rx, int ry, int rz, std::vector<proto::ColTri>& out)
{
    std::lock_guard lk(g_mutex);
    auto it = g_cols.find(ColKey(rx, rz));
    if (it == g_cols.end())
        return;
    auto r = it->second.regions.find(ry);
    if (r != it->second.regions.end())
        out.insert(out.end(), r->second.begin(), r->second.end());
}

void TakeChanged(std::vector<std::pair<int, int>>& out)
{
    std::lock_guard lk(g_mutex);
    for (uint64_t k : g_changed)
        out.emplace_back(KeyX(k), KeyZ(k));
    g_changed.clear();
}

bool Active()
{
    return g_active.load(std::memory_order_relaxed);
}

void SetSourceActive(bool on)
{
    g_active.store(on, std::memory_order_relaxed);
}

// ---- M6b: motion, flight config, corridor, scan planner ------------------------------------
namespace
{
std::mutex g_motionMutex;
Motion g_motion;
FlightConfig g_flight;

struct ScanRec
{
    uint64_t lastMs = 0;
    int ryLo = 0, ryHi = -1;
    bool terrain = false;
};
std::mutex g_planMutex;
std::unordered_map<uint64_t, ScanRec> g_scanRecs;

// Horizontal distance from (px, pz) to region column (rx, rz)'s 8x8 square.
float RectDist(int rx, int rz, double px, double pz)
{
    const double x0 = rx * 8.0, z0 = rz * 8.0;
    const double dx = px < x0 ? x0 - px : (px > x0 + 8 ? px - x0 - 8 : 0.0);
    const double dz = pz < z0 ? z0 - pz : (pz > z0 + 8 ? pz - z0 - 8 : 0.0);
    return static_cast<float>(std::sqrt(dx * dx + dz * dz));
}
// Time step along the path: points at most ~2 blocks and 0.05 s apart.
double PathStep(const Motion& m)
{
    const double hs = m.HSpeed();
    return hs > 40.0 ? 2.0 / hs : 0.05;
}
} // namespace

double Motion::Speed() const
{
    return std::sqrt(vx * vx + vy * vy + vz * vz);
}
double Motion::HSpeed() const
{
    return std::sqrt(vx * vx + vz * vz);
}

void SetMotion(const Motion& m)
{
    std::lock_guard lk(g_motionMutex);
    g_motion = m;
}

bool GetMotion(Motion& out, uint64_t nowMs)
{
    std::lock_guard lk(g_motionMutex);
    out = g_motion;
    if (!out.valid || nowMs > out.ms + 500)
        out.valid = false;
    return out.valid;
}

Motion g_focus; // guarded by g_motionMutex

void SetFocus(const Motion& m)
{
    std::lock_guard lk(g_motionMutex);
    g_focus = m;
}

bool GetFocus(Motion& out, uint64_t nowMs)
{
    std::lock_guard lk(g_motionMutex);
    out = g_focus;
    if (!out.valid || nowMs > out.ms + 500)
        out.valid = false;
    return out.valid;
}

void SetFlightConfig(const FlightConfig& c)
{
    std::lock_guard lk(g_motionMutex);
    g_flight = c;
}

FlightConfig GetFlightConfig()
{
    std::lock_guard lk(g_motionMutex);
    return g_flight;
}

float Horizon(const Motion& m)
{
    const FlightConfig f = GetFlightConfig();
    if (!m.valid || f.lookaheadMs <= 0)
        return 0.0f;
    const double sp = m.Speed();
    double h = f.lookaheadMs / 1000.0;
    if (sp * h > f.maxAhead)
        h = f.maxAhead / sp;
    return static_cast<float>(h);
}

float ColumnEta(int rx, int rz, const Motion& m, float horizonS, float margin)
{
    if (!m.valid || horizonS <= 0.0f)
        return RectDist(rx, rz, m.x, m.z) <= margin ? 0.0f : INFINITY;
    const double dt = PathStep(m);
    for (double t = 0.0;; t += dt)
    {
        const double tt = std::min(t, static_cast<double>(horizonS));
        if (RectDist(rx, rz, m.x + m.vx * tt, m.z + m.vz * tt) <= margin)
            return static_cast<float>(tt);
        if (tt >= horizonS)
            break;
    }
    return INFINITY;
}

void CorridorColumns(const Motion& m, float horizonS, float margin, std::vector<std::pair<uint64_t, float>>& out)
{
    std::unordered_map<uint64_t, float> eta;
    const double dt = PathStep(m);
    const int reach = static_cast<int>(std::ceil(margin / 8.0f)) + 1;
    for (double t = 0.0;; t += dt)
    {
        const double tt = std::min(t, static_cast<double>(horizonS));
        const double px = m.x + (m.valid ? m.vx * tt : 0.0), pz = m.z + (m.valid ? m.vz * tt : 0.0);
        const int crx = FloorDiv(static_cast<int>(std::floor(px)), kRegion), crz = FloorDiv(static_cast<int>(std::floor(pz)), kRegion);
        for (int dx = -reach; dx <= reach; ++dx)
            for (int dz = -reach; dz <= reach; ++dz)
                if (RectDist(crx + dx, crz + dz, px, pz) <= margin)
                {
                    auto [it, ins] = eta.try_emplace(PackKey(crx + dx, crz + dz), static_cast<float>(tt));
                    if (!ins)
                        it->second = std::min(it->second, static_cast<float>(tt));
                }
        if (tt >= horizonS || !m.valid)
            break;
    }
    out.assign(eta.begin(), eta.end());
}

namespace
{
struct GateWant
{
    bool active = false;
    int rx = 0, rz = 0, radius = 0;
    uint64_t sinceMs = 0;
};
GateWant g_gate; // guarded by g_motionMutex
} // namespace

void SetGate(bool active, double x, double z, int radius, uint64_t sinceMs)
{
    GateWant g;
    g.active = active;
    g.rx = FloorDiv(static_cast<int>(std::floor(x)), kRegion);
    g.rz = FloorDiv(static_cast<int>(std::floor(z)), kRegion);
    g.radius = radius;
    g.sinceMs = sinceMs;
    std::lock_guard lk(g_motionMutex);
    g_gate = g;
}

void ClearPlan()
{
    std::lock_guard lk(g_planMutex);
    g_scanRecs.clear();
}

void MarkScanned(int rx, int rz, int ryLo, int ryHi, bool terrain, uint64_t nowMs)
{
    std::lock_guard lk(g_planMutex);
    g_scanRecs[PackKey(rx, rz)] = {nowMs, ryLo, ryHi, terrain};
}

void PlanScan(double cx, double cy, double cz, const Motion& m, const PlanSettings& s, uint64_t nowMs, std::vector<PlanItem>& out,
              const Motion* focus)
{
    out.clear();
    const int prx = FloorDiv(static_cast<int>(std::floor(cx)), kRegion), prz = FloorDiv(static_cast<int>(std::floor(cz)), kRegion);
    const float horizon = Horizon(m);
    const double speed = m.valid ? m.Speed() : 0.0;
    const bool fast = speed > 8.0;
    // Vertical range: around the feet now and where the path goes within the horizon (dives).
    const double yEnd = cy + (m.valid ? m.vy * horizon : 0.0);
    int needLo = FloorDiv(static_cast<int>(std::floor(std::min(cy, yEnd))) - s.below, kRegion);
    int needHi = FloorDiv(static_cast<int>(std::floor(std::max(cy, yEnd))) + s.above, kRegion);
    // Where the body is within 0.5 s: a column that doesn't cover that is a "must".
    const double ySoon = cy + (m.valid ? m.vy * 0.5 : 0.0);
    const int soonLo = FloorDiv(static_cast<int>(std::floor(std::min(cy, ySoon))) - 3, kRegion);
    const int soonHi = FloorDiv(static_cast<int>(std::floor(std::max(cy, ySoon))) + 3, kRegion);
    constexpr int kMaxSpan = 16; // regions (128 blocks) per scan box
    if (needHi - needLo + 1 > kMaxSpan)
    {
        // Keep the side we move to (a dive needs what is below, not 20 blocks above).
        if (m.valid && m.vy < 0.0)
            needHi = std::max(soonHi, needLo + kMaxSpan - 1);
        needLo = needHi - kMaxSpan + 1;
    }
    // Slack in flight on the side we move to (half a second of vertical travel), so a climb or a
    // dive doesn't rescan the same columns every 8 blocks.
    const int slack = fast ? 1 + static_cast<int>(std::fabs(m.vy) * 0.5 / kRegion) : 0;

    // Candidates: the square around the feet (as M5a) + the flight corridor ahead (M6b) + an ender
    // pearl's path (each with the heights it needs).
    struct Want
    {
        float eta;
        int lo, hi, soonLo, soonHi;
    };
    std::unordered_map<uint64_t, Want> cand; // key -> what that column needs
    auto want = [&](uint64_t key, const Want& w) {
        auto [it, ins] = cand.try_emplace(key, w);
        if (!ins)
        {
            Want& o = it->second;
            o.eta = std::min(o.eta, w.eta);
            o.lo = std::min(o.lo, w.lo), o.hi = std::max(o.hi, w.hi);
            if (o.hi - o.lo + 1 > kMaxSpan)
                o.lo = w.lo, o.hi = w.hi; // can't have both: the newer need (the pearl) wins
            o.soonLo = std::min(o.soonLo, w.soonLo), o.soonHi = std::max(o.soonHi, w.soonHi);
        }
    };
    const double slow = std::max(speed, 4.0);
    for (int dx = -s.radius; dx <= s.radius; ++dx)
        for (int dz = -s.radius; dz <= s.radius; ++dz)
        {
            const int d = std::max(std::abs(dx), std::abs(dz));
            want(PackKey(prx + dx, prz + dz), {static_cast<float>(std::max(0, d - 1) * 8.0 / slow), needLo, needHi, soonLo, soonHi});
        }
    if (horizon > 0.0f)
    {
        std::vector<std::pair<uint64_t, float>> cor;
        CorridorColumns(m, horizon, 6.0f, cor);
        for (auto& c : cor)
            want(c.first, {c.second, needLo, needHi, soonLo, soonHi});
    }
    if (focus && focus->valid)
    {
        // The pearl's path for 1.5 s: from where it is down to where gravity (~12 blocks/s^2)
        // takes it, plus the usual margin below for the ground it lands on.
        const Motion& f = *focus;
        const float fh = 1.5f;
        const double yLow = std::min(f.y, f.y + f.vy * fh - 0.5 * 12.0 * fh * fh), yHigh = std::max(f.y, f.y + f.vy * fh);
        int fLo = FloorDiv(static_cast<int>(std::floor(yLow)) - s.below, kRegion);
        const int fHi = FloorDiv(static_cast<int>(std::floor(yHigh)) + 3, kRegion);
        // Perf: also down to the ground the player stands on. A pearl high above the
        // ground (thrown from a roof, a tower or upwards) otherwise only had its own heights scanned:
        // no Witcher surface in that box, so Minecraft held it the full 3 s and landed it in mid-air.
        // Up to 24 regions (192 blocks; the host sends at most 24 per column).
        constexpr int kMaxFocusSpan = 24;
        fLo = std::min(fLo, needLo);
        fLo = std::max(fLo, fHi - kMaxFocusSpan + 1);
        const int fsLo = FloorDiv(static_cast<int>(std::floor(f.y + std::min(0.0, f.vy * 0.5) - 3.0)) - 3, kRegion);
        const int fsHi = FloorDiv(static_cast<int>(std::floor(f.y)) + 3, kRegion);
        std::vector<std::pair<uint64_t, float>> cor;
        CorridorColumns(f, fh, 6.0f, cor);
        for (auto& c : cor)
            want(c.first, {c.second, fLo, fHi, fsLo, fsHi});
    }

    GateWant gate;
    {
        std::lock_guard glk(g_motionMutex);
        gate = g_gate;
    }
    if (gate.active)
        for (int dx = -gate.radius; dx <= gate.radius; ++dx)
            for (int dz = -gate.radius; dz <= gate.radius; ++dz)
                want(PackKey(gate.rx + dx, gate.rz + dz), {0.0f, needLo, needHi, soonLo, soonHi});

    std::lock_guard lk(g_planMutex);
    struct Tmp
    {
        PlanItem item;
        uint64_t age;
    };
    std::vector<Tmp> tmp;
    for (auto& kv : cand)
    {
        const int rx = KeyX(kv.first), rz = KeyZ(kv.first);
        const int d = std::max(std::abs(rx - prx), std::abs(rz - prz));
        const Want& w = kv.second;
        auto it = g_scanRecs.find(kv.first);
        const bool never = it == g_scanRecs.end();
        const bool rangeMoved = !never && (w.lo < it->second.ryLo || w.hi > it->second.ryHi);
        const bool soonMissing = never || w.soonLo < it->second.ryLo || w.soonHi > it->second.ryHi;
        const uint64_t age = never ? UINT64_MAX : nowMs - it->second.lastMs;
        uint64_t refresh = static_cast<uint64_t>(s.refreshMs) * (d <= 1 ? 2 : 5) / 2;
        // W3 streams its world in around Geralt: a column ahead with no terrain yet is retried soon.
        if (!never && fast && !it->second.terrain && w.eta < horizon)
            refresh = std::min<uint64_t>(refresh, 300);
        // Perf: the hold gate waits for a scan of this column newer than its start: do it now.
        const bool gateDue = gate.active && !never && it->second.lastMs < gate.sinceMs && std::abs(rx - gate.rx) <= gate.radius &&
                             std::abs(rz - gate.rz) <= gate.radius;
        if (!(never || rangeMoved || gateDue || age >= refresh))
            continue;
        int lo = w.lo, hi = w.hi;
        if (fast)
        {
            const int sLo = m.vy < -2.0 ? slack : 0, sHi = m.vy > 2.0 ? slack : 0;
            if (hi - lo + 1 + sLo + sHi <= kMaxSpan)
                lo -= sLo, hi += sHi;
            // Keep what was already scanned when it fits (no data dropped from under the player).
            if (!never && std::max(hi, it->second.ryHi) - std::min(lo, it->second.ryLo) + 1 <= kMaxSpan)
                lo = std::min(lo, it->second.ryLo), hi = std::max(hi, it->second.ryHi);
        }
        tmp.push_back({{rx, rz, lo, hi, w.eta, never || rangeMoved || gateDue, soonMissing && w.eta <= 0.5f}, age});
    }
    std::sort(tmp.begin(), tmp.end(), [](const Tmp& a, const Tmp& b) {
        if (a.item.must != b.item.must)
            return a.item.must;
        if (a.item.urgent != b.item.urgent)
            return a.item.urgent;
        if (a.item.eta != b.item.eta)
            return a.item.eta < b.item.eta;
        return a.age > b.age;
    });
    for (auto& t : tmp)
        out.push_back(t.item);
    // Forget records far from both the feet and the corridor (rescanned when we come back).
    for (auto it = g_scanRecs.begin(); it != g_scanRecs.end();)
    {
        const int rx = KeyX(it->first), rz = KeyZ(it->first);
        if ((std::abs(rx - prx) > s.radius + 3 || std::abs(rz - prz) > s.radius + 3) && !cand.count(it->first))
            it = g_scanRecs.erase(it);
        else
            ++it;
    }
}

std::string StoreStatus()
{
    size_t cols = 0, tris = 0;
    {
        std::lock_guard lk(g_mutex);
        cols = g_cols.size();
        for (auto& kv : g_cols)
            tris += kv.second.tris;
    }
    char buf[160];
    std::snprintf(buf, sizeof(buf), "obstacles: %s, %zu columns, %zu tris, %llu scans, %llu changes", Active() ? "on" : "off", cols, tris,
                  static_cast<unsigned long long>(g_sets.load()), static_cast<unsigned long long>(g_changes.load()));
    return buf;
}
} // namespace wc::collision
