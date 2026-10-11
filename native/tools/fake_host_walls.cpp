// ---- M5a offline test: W3 walls and buildings through the real Minecraft ----------------------
//
//   fake_host.exe walls [timeoutSeconds=300]
//
// Streams fake W3 obstacle geometry through the SAME path the native PhysX scanner uses
// (collision::SetColumn -> collision store -> skycraft_host's SendColumn merge, hold gate, voxels):
//   - a 12 m x 0.4 m x 3 m WALL 5 m north of the start,
//   - a 6 x 6 m box HUT (0.3 m walls, a roof, a 1.4 x 2.2 m door gap in its south wall) 24 m east,
//     with a 1.2 x 1.2 x 0.5 m CRATE inside, between the door and the north wall.
// The script's ground sampler is emulated too (it hits the crate top, not the 3 m walls).
// Minecraft mode (as the control test), survival:
//   1. W for 4 s towards the wall: Steve must stop 0.3 m (his radius) in front of it.
//   2. Teleport (kTickTeleport, through the hold gate) 3 m south of the hut's door; W for 6 s:
//      Steve must pass the door gap, step onto the crate (feet +0.5 m), and stop at the north wall
//      inside the hut.
// Exit code 0 = PASS.
#include "buildmode.hpp"
#include "collision.hpp"
#include "control.hpp"
#include "log.hpp"
#include "skycraft_host.hpp"
#include "skycraft_protocol.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
namespace proto = skycraft::proto;

struct BoxW3 // axis-aligned, W3 coords
{
    float x0, y0, z0, x1, y1, z1;
};

int FloorDiv8(int v)
{
    return v >= 0 ? v / 8 : -((-v + 7) / 8);
}

// The 12 triangles of a W3 box in MC coordinates (x, z, -y).
void BoxTris(const BoxW3& b, std::vector<proto::ColTri>& out)
{
    auto mc = [](float x, float y, float z, float* o) {
        o[0] = x;
        o[1] = z;
        o[2] = -y;
    };
    float c[8][3];
    for (int i = 0; i < 8; ++i)
        mc((i & 1) ? b.x1 : b.x0, (i & 2) ? b.y1 : b.y0, (i & 4) ? b.z1 : b.z0, c[i]);
    static const int f[6][4] = {{0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5}};
    for (const auto& q : f)
    {
        const int tri[2][3] = {{q[0], q[1], q[2]}, {q[0], q[2], q[3]}};
        for (const auto& t : tri)
        {
            proto::ColTri ct{};
            for (int k = 0; k < 3; ++k)
                for (int a = 0; a < 3; ++a)
                    ct.v[k * 3 + a] = c[t[k]][a];
            out.push_back(ct);
        }
    }
}

volatile bool g_wallsQuit = false;
BOOL WINAPI OnCtrlWalls(DWORD)
{
    g_wallsQuit = true;
    return TRUE;
}
} // namespace

int RunWallsTest(int timeout)
{
    using namespace wc;
    SetConsoleCtrlHandler(OnCtrlWalls, TRUE);
    const float G = 64.0f;
    const uint64_t seed = (GetTickCount64() / 1000) % 200;
    const float sx = -5000.0f - static_cast<float>(seed) * 150.0f + 0.5f, sy = 400.5f;
    const float wallY = sy + 5.0f;
    const float hx = sx + 24.0f, hy0 = sy + 6.0f; // hut: interior x [hx-3, hx+3], y [hy0, hy0+6]
    const float crateY0 = hy0 + 2.0f, crateY1 = hy0 + 3.2f;
    std::vector<BoxW3> boxes = {
        {sx - 6, wallY, G, sx + 6, wallY + 0.4f, G + 3},                  // the wall
        {hx - 3.3f, hy0 - 0.3f, G, hx - 0.7f, hy0, G + 3},                // hut south wall, west of the door
        {hx + 0.7f, hy0 - 0.3f, G, hx + 3.3f, hy0, G + 3},                // ... east of the door
        {hx - 0.7f, hy0 - 0.3f, G + 2.2f, hx + 0.7f, hy0, G + 3},         // ... above the door
        {hx - 3.3f, hy0 + 6, G, hx + 3.3f, hy0 + 6.3f, G + 3},            // north wall
        {hx - 3.3f, hy0, G, hx - 3, hy0 + 6, G + 3},                      // west wall
        {hx + 3, hy0, G, hx + 3.3f, hy0 + 6, G + 3},                      // east wall
        {hx - 3.3f, hy0 - 0.3f, G + 3, hx + 3.3f, hy0 + 6.3f, G + 3.3f}, // roof
        {hx - 0.6f, crateY0, G, hx + 0.6f, crateY1, G + 0.5f},           // crate
    };
    std::vector<proto::ColTri> all;
    for (const auto& b : boxes)
        BoxTris(b, all);
    WLOG("walls test: start W3 (%.1f, %.1f, %.1f); wall at y %.1f; hut door at (%.1f, %.1f), crate y %.1f..%.1f; %zu triangles; timeout %d s", sx, sy,
         G, wallY, hx, hy0, crateY0, crateY1, all.size(), timeout);

    // The script's ground sampler: highest box top under (x, y) no higher than feet + 2.5, else G.
    auto groundAt = [&](int wx, int wy, float feetZ) {
        float h = G;
        for (const auto& b : boxes)
            if (wx >= b.x0 && wx <= b.x1 && wy >= b.y0 && wy <= b.y1 && b.z1 <= feetZ + 2.5f)
                h = std::max(h, b.z1);
        return h;
    };
    // The native scanner's job: every region column within 2 of the centre gets the triangles that
    // touch it, for regions [feet-12, feet+20] (as collision_physx.cpp). Unchanged data doesn't
    // bump the store version; the scan time feeds the hold gate.
    auto scan = [&](float cx, float cy, float cz) {
        const double mx = cx, my = cz, mz = -cy;
        const int prx = FloorDiv8(static_cast<int>(std::floor(mx))), prz = FloorDiv8(static_cast<int>(std::floor(mz)));
        const int fy = static_cast<int>(std::floor(my));
        const int ryLo = FloorDiv8(fy - 12), ryHi = FloorDiv8(fy + 20);
        std::vector<proto::ColTri> col;
        for (int rx = prx - 2; rx <= prx + 2; ++rx)
            for (int rz = prz - 2; rz <= prz + 2; ++rz)
            {
                col.clear();
                const float lo[3] = {rx * 8.0f, ryLo * 8.0f, rz * 8.0f}, hi[3] = {rx * 8.0f + 8, ryHi * 8.0f + 8, rz * 8.0f + 8};
                for (const auto& t : all)
                {
                    bool in = true;
                    for (int k = 0; k < 3 && in; ++k)
                    {
                        const float a = std::min({t.v[k], t.v[3 + k], t.v[6 + k]}), b = std::max({t.v[k], t.v[3 + k], t.v[6 + k]});
                        in = b >= lo[k] - 0.01f && a <= hi[k] + 0.01f;
                    }
                    if (in)
                        col.push_back(t);
                }
                collision::SetColumn(rx, rz, ryLo, ryHi, col.data(), col.size(), GetTickCount64());
            }
    };

    control::SetMcMode(true);
    if (!host::Start())
        return 1;
    host::SetWorld("fake\\walls_test.w2w");
    collision::SetSourceActive(true);

    enum Phase
    {
        WaitDriven,
        Survival,
        Wall,
        Teleport,
        Hut,
        Done,
        Failed
    };
    const char* names[] = {"WaitDriven", "Survival", "Wall", "Teleport", "Hut", "Done", "Failed"};
    Phase phase = WaitDriven;
    double phaseT = 0;
    std::string fails;
    auto fail = [&](const char* what) {
        WLOG("walls test: FAIL: %s", what);
        fails += what;
        fails += "; ";
    };
    float geraltX = sx, geraltY = sy, geraltZ = G;
    bool followMc = true, sendTeleport = false;
    int scanIndex = 0;
    const double t0 = GetTickCount64() / 1000.0;
    double lastStep = -1, lastPrint = -1, lastScan = -1;
    double p0y = 0, maxY = -1e9, crateZ = 0, minZ = 1e9;
    bool wallOk = false, doorOk = false, crateOk = false, northOk = false, sawGate = false;
    uint64_t holds0 = 0;

    while (!g_wallsQuit && phase != Done && phase != Failed)
    {
        const double t = GetTickCount64() / 1000.0 - t0;
        if (t > timeout)
        {
            WLOG("walls test: TIMEOUT in phase %s", names[phase]);
            fail("timeout");
            phase = Failed;
            break;
        }
        const control::Frame fr = control::OnFrame(0, 0.0f, 0.0f);
        const auto st = host::GetStatus();
        const bool inWorld = st.mcAlive && st.haveMcState && (st.mcFlags & proto::kMcInWorld);
        const double mcX = st.mcX, mcY = -st.mcZ, mcZ = st.mcY; // MC feet in W3 coords
        if (st.gateHolding && phase == Teleport)
            sawGate = true;

        if (t - lastStep >= 0.033)
        {
            lastStep = t;
            if (followMc && fr.engaged && fr.eyeValid && fr.arrived)
            {
                geraltX = static_cast<float>(fr.feet[0]);
                geraltY = static_cast<float>(fr.feet[1]);
                geraltZ = static_cast<float>(fr.feet[2]);
            }
            float cx = geraltX, cy = geraltY, cz = geraltZ;
            host::SampleCentre(cx, cy, cz);
            std::string batch;
            const int bx = static_cast<int>(std::floor(cx)), by = static_cast<int>(std::floor(cy));
            for (int n = 0; n < 160; ++n)
            {
                const int idx = scanIndex++ % (33 * 33);
                const int wx = bx + idx % 33 - 16, wy = by + idx / 33 - 16;
                char item[64];
                std::snprintf(item, sizeof(item), "%d,%d,%.3f;", wx, wy, groundAt(wx, wy, cz));
                batch += item;
            }
            host::PushHeights(batch);
            if (t - lastScan >= 0.5) // the script calls WCraft_ColScan every step; a full refresh every ~1.5 s
            {
                lastScan = t;
                scan(cx, cy, cz);
            }
            uint32_t flags = host::kTickInGame;
            if (sendTeleport)
            {
                flags |= host::kTickTeleport;
                sendTeleport = false;
            }
            host::Tick(geraltX, geraltY, geraltZ, 180.0f - st.mcYaw, -st.mcPitch, flags);
        }

        switch (phase)
        {
        case WaitDriven:
            if (st.driven && inWorld && st.teleportAck == st.teleportSeq && !st.gateHolding && fr.eyeValid)
            {
                WLOG("walls test: Minecraft drives (t=%.1f s) at W3 (%.2f, %.2f, %.2f)", t, mcX, mcY, mcZ);
                buildmode::QueueCommand("gamemode survival");
                phase = Survival;
                phaseT = t;
            }
            break;
        case Survival:
            if (t - phaseT > 3.0 && buildmode::CommandsPending() == 0 && !(st.mcFlags & proto::kMcScreenOpen))
            {
                p0y = mcY;
                maxY = mcY;
                WLOG("walls test: WALL: W down for 4 s from (%.2f, %.2f, %.2f) towards the wall at y %.2f", mcX, mcY, mcZ, wallY);
                buildmode::QueueEvent({proto::kInKey, 26, 1, 0, 0});
                phase = Wall;
                phaseT = t;
            }
            break;
        case Wall:
            maxY = std::max(maxY, mcY);
            if (t - phaseT > 4.0)
            {
                buildmode::QueueEvent({proto::kInKey, 26, 0, 0, 0});
                // Steve's radius is 0.3: his feet stop at wallY - 0.3.
                wallOk = mcY - p0y > 3.0 && maxY < wallY - 0.2 && mcY > wallY - 0.6 && std::fabs(mcZ - G) < 0.1;
                WLOG("walls test: walked %.2f m north, max y %.2f, now (%.2f, %.2f, %.2f); wall face at %.2f -> %s", mcY - p0y, maxY, mcX, mcY, mcZ,
                     wallY, wallOk ? "STOPPED AT THE WALL (OK)" : "BAD");
                if (!wallOk)
                    fail(maxY >= wallY ? "walked through the wall" : "wall stop position");
                // Next: the hut. Geralt jumps there (as a quest teleport); Minecraft follows through the gate.
                followMc = false;
                geraltX = hx;
                geraltY = hy0 - 3.0f;
                geraltZ = G;
                sendTeleport = true;
                holds0 = st.gateHolds;
                phase = Teleport;
                phaseT = t;
            }
            break;
        case Teleport:
            if (t - phaseT > 1.0 && inWorld && st.teleportAck == st.teleportSeq && !st.gateHolding &&
                std::fabs(mcX - hx) < 0.6 && std::fabs(mcY - (hy0 - 3.0)) < 0.6 && fr.arrived)
            {
                WLOG("walls test: at the hut (t=%.1f s) (%.2f, %.2f, %.2f); hold gate seen %s (holds %llu -> %llu)", t, mcX, mcY, mcZ,
                     sawGate ? "yes" : "no", static_cast<unsigned long long>(holds0), static_cast<unsigned long long>(st.gateHolds));
                followMc = true;
                p0y = mcY;
                maxY = mcY;
                crateZ = 0;
                minZ = mcZ;
                buildmode::QueueEvent({proto::kInKey, 26, 1, 0, 0});
                WLOG("walls test: HUT: W down for 6 s through the door (x %.1f..%.1f) at y %.2f", hx - 0.7, hx + 0.7, hy0);
                phase = Hut;
                phaseT = t;
            }
            else if (t - phaseT > 30.0)
            {
                fail("teleport to the hut");
                phase = Failed;
            }
            break;
        case Hut:
            maxY = std::max(maxY, mcY);
            minZ = std::min(minZ, mcZ);
            if (mcY > crateY0 + 0.2 && mcY < crateY1 - 0.2)
                crateZ = std::max(crateZ, mcZ - G);
            if (t - phaseT > 6.0)
            {
                buildmode::QueueEvent({proto::kInKey, 26, 0, 0, 0});
                doorOk = maxY > hy0 + 0.5;
                crateOk = crateZ > 0.4 && crateZ < 0.6;
                northOk = maxY < hy0 + 6.0 - 0.2 && mcY > hy0 + 6.0 - 0.6;
                WLOG("walls test: through the door: %s (max y %.2f, door at %.2f); on the crate: %s (feet +%.2f m); stopped at the north wall: %s "
                     "(y %.2f, face %.2f); now (%.2f, %.2f, %.2f), lowest z %.2f",
                     doorOk ? "yes" : "NO", maxY, hy0, crateOk ? "yes" : "NO", crateZ, northOk ? "yes" : "NO", mcY, hy0 + 6.0, mcX, mcY, mcZ, minZ);
                if (!doorOk)
                    fail("door");
                if (!crateOk)
                    fail("crate");
                if (!northOk)
                    fail("north wall");
                phase = Done;
            }
            break;
        default:
            break;
        }
        if (t - lastPrint >= 2.0)
        {
            lastPrint = t;
            WLOG("t=%5.1fs %s | MC (%.2f, %.2f, %.2f) | %s | %s", t, names[phase], mcX, mcY, mcZ, host::StatusLine().c_str(),
                 collision::StoreStatus().c_str());
        }
        Sleep(16);
    }
    for (int i = 0; i < 30; ++i)
    {
        control::OnFrame(0, 0.0f, 0.0f);
        host::Tick(geraltX, geraltY, geraltZ, 0.0f, 0.0f, host::kTickInGame);
        Sleep(33);
    }
    const bool pass = phase == Done && fails.empty() && wallOk && doorOk && crateOk && northOk;
    WLOG("walls test: %s (wall %d door %d crate %d northWall %d gate %d) %s; %s", pass ? "PASS" : "FAIL", wallOk, doorOk, crateOk, northOk, sawGate,
         fails.c_str(), host::StatusLine().c_str());
    host::Stop();
    return pass ? 0 : 2;
}

// ---- M6b offline test: solid world at flight speed (no tunnelling) --------------------------------
//
//   fake_host.exe flight [timeoutSeconds=300]          (set WC_FLIGHT_LEGACY=1 for the M5a behaviour)
//
// A fake W3 world streamed through the real planner (collision::PlanScan, the same scheduling the
// native PhysX scanner uses, with a W3-like per-column cost), the host's streaming, the flight guard
// and an emulation of the script's ground sampler (witchercraft.ws, 96 traces per step around
// WCraft_SampleCentre). The terrain is a flat W3 PhysX height field (kTriTerrain) everywhere.
//   A. RUN: movement_speed x10 (~43 blocks/s) for 6 s towards a 0.4 m thick wall 150 m north:
//      Steve must stop in front of it.
//   B. FALL: teleport onto a 2 x 2 m tower 120 m tall, run off it at speed and fall 120 m:
//      Steve must land ON the terrain (never below it, no safety-net rescue).
//   C. PEARL (last): throw an ender pearl 30 degrees up (lands ~40 blocks away, past the collision
//      around Steve): Steve must be teleported more than 20 m and stand on the terrain.
//   0. WALK (first): sprint and jump at normal survival speed for 12 s, with the guard's speed
//      floor lowered to 3 blocks/s: no flight-guard hold and no hold gate (in game, rescans every
//      CollisionRefreshMs made the guard freeze Steve for ~1.5 s on every jump).
// Exit code 0 = PASS.
int RunFlightTest(int timeout)
{
    using namespace wc;
    HANDLE frameTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    SetConsoleCtrlHandler(OnCtrlWalls, TRUE);
    const char* legacyEnv = std::getenv("WC_FLIGHT_LEGACY");
    const bool legacy = legacyEnv && std::atoi(legacyEnv) != 0;
    collision::FlightConfig fc = collision::GetFlightConfig();
    if (legacy)
        fc.lookaheadMs = 0; // M5a: square around the feet only, no corridor, no guard, no lead
    collision::SetFlightConfig(fc);
    const float G = 64.0f;
    const uint64_t seed = (GetTickCount64() / 1000) % 200;
    const float sx = 8000.0f + static_cast<float>(seed) * 300.0f + 0.5f, sy = -600.5f;
    const float wallY = sy + 150.0f, towerX = sx + 40.0f, towerTop = G + 120.0f;
    std::vector<BoxW3> boxes = {
        {sx - 20, wallY, G, sx + 20, wallY + 0.4f, G + 8},     // the wall
        {towerX - 1, sy - 1, G, towerX + 1, sy + 1, towerTop}, // the tower
        {sx + 20, sy + 30, G, sx + 24, sy + 34, G + 2.5f},      // M7 CLIMB: a 2.5 m crate
    };
    std::vector<proto::ColTri> boxTris;
    for (const auto& b : boxes)
        BoxTris(b, boxTris);
    // M7 SWIM: a flooded field (W3 water surface 3 m over the ground) west of the start.
    const float poolX0 = sx - 60.0f, poolX1 = sx - 30.0f, poolY0 = sy - 15.0f, poolY1 = sy + 15.0f, poolW = G + 3.0f;
    auto waterAt = [&](int wx, int wy) { return wx >= poolX0 && wx <= poolX1 && wy >= poolY0 && wy <= poolY1 ? poolW : NAN; };
    WLOG("flight test (M6b%s): start W3 (%.1f, %.1f, %.1f); wall at y %.1f; tower at (%.1f, %.1f) top z %.1f; timeout %d s",
         legacy ? ", LEGACY M5a behaviour" : "", sx, sy, G, wallY, towerX, sy, towerTop, timeout);

    // The script's sampler (witchercraft.ws): from feet + 2.5 m down 250 m (M5a: 30 m); else from
    // feet + 40 m down to feet + 2.5 m; else "no ground".
    auto groundAt = [&](int wx, int wy, float feetZ) {
        const float down = legacy ? 30.0f : 250.0f;
        auto hitTop = [&](float lo, float hi) {
            float h = -1e9f;
            if (G >= lo && G <= hi)
                h = G;
            for (const auto& b : boxes)
                if (wx >= b.x0 && wx <= b.x1 && wy >= b.y0 && wy <= b.y1 && b.z1 >= lo && b.z1 <= hi)
                    h = std::max(h, b.z1);
            return h;
        };
        float best = hitTop(feetZ - down, feetZ + 2.5f);
        if (best < -1e8f)
            best = hitTop(feetZ + 2.5f, feetZ + 40.0f);
        return best < -1e8f ? NAN : best;
    };
    // W3's PhysX for one region column: the height field (2 m cells) + the boxes, within the box.
    auto extract = [&](int rx, int rz, int ryLo, int ryHi, std::vector<proto::ColTri>& col, bool& terrain) {
        col.clear();
        const float lo[3] = {rx * 8.0f, ryLo * 8.0f, rz * 8.0f}, hi[3] = {rx * 8.0f + 8, ryHi * 8.0f + 8, rz * 8.0f + 8};
        terrain = G >= lo[1] && G <= hi[1];
        if (terrain)
            for (int i = 0; i < 4; ++i)
                for (int k = 0; k < 4; ++k)
                {
                    const float x0 = lo[0] + i * 2.0f, z0 = lo[2] + k * 2.0f;
                    col.push_back({{x0, G, z0, x0 + 2, G, z0, x0 + 2, G, z0 + 2}, proto::kTriTerrain});
                    col.push_back({{x0, G, z0, x0 + 2, G, z0 + 2, x0, G, z0 + 2}, proto::kTriTerrain});
                }
        for (const auto& t : boxTris)
        {
            bool in = true;
            for (int k = 0; k < 3 && in; ++k)
            {
                const float a = std::min({t.v[k], t.v[3 + k], t.v[6 + k]}), b = std::max({t.v[k], t.v[3 + k], t.v[6 + k]});
                in = b >= lo[k] - 0.01f && a <= hi[k] + 0.01f;
            }
            if (in)
                col.push_back(t);
        }
        // WC_DENSE=<tris per column>: Novigrad-like clutter in every column,
        // all of it UNDER the ground (W3 z G-11..G-1) so the test's walking/flying is unchanged: 4 big
        // building walls 20 m x 0.4 m x 8 m (they reach far out of the column, as W3's box shapes do)
        // and small crates / mesh bits (0.2-1.2 m) for the rest. In game: ~2600 tris per column.
        static const int dense = std::getenv("WC_DENSE") ? std::atoi(std::getenv("WC_DENSE")) : 0;
        if (dense > 0 && lo[1] <= G - 1.0f && hi[1] >= G - 11.0f)
        {
            uint32_t h = static_cast<uint32_t>(rx) * 73856093u ^ static_cast<uint32_t>(rz) * 19349663u;
            auto rnd = [&h]() {
                h ^= h << 13, h ^= h >> 17, h ^= h << 5;
                return (h & 0xFFFFFF) / 16777216.0f;
            };
            std::vector<proto::ColTri> clutter;
            const float cx = rx * 8.0f + 4.0f, cz = rz * 8.0f + 4.0f; // MC
            for (int w = 0; w < 4; ++w)
            {
                // W3 box: x = MC x, y = -MC z, z = MC y
                const float ox = cx + (rnd() - 0.5f) * 8.0f, oz = cz + (rnd() - 0.5f) * 8.0f;
                const float z0 = G - 11.0f + rnd() * 2.0f;
                if (w & 1)
                    BoxTris({ox - 10.0f, -oz - 0.2f, z0, ox + 10.0f, -oz + 0.2f, z0 + 8.0f}, clutter);
                else
                    BoxTris({ox - 0.2f, -oz - 10.0f, z0, ox + 0.2f, -oz + 10.0f, z0 + 8.0f}, clutter);
            }
            while (static_cast<int>(clutter.size()) + 12 <= dense)
            {
                const float s = 0.2f + rnd() * 1.0f;
                const float ox = rx * 8.0f + rnd() * 8.0f, oz = rz * 8.0f + rnd() * 8.0f, z0 = G - 11.0f + rnd() * (10.0f - s);
                BoxTris({ox, -oz - s, z0, ox + s, -oz, z0 + s}, clutter);
            }
            for (const auto& t : clutter)
                if (std::max({t.v[1], t.v[4], t.v[7]}) >= lo[1] && std::min({t.v[1], t.v[4], t.v[7]}) <= hi[1])
                    col.push_back(t);
        }
    };
    // WCraft_ColScan: the same planner and budget rules as collision_physx.cpp; each column costs
    // ~300 us of game-thread time (the in-game average was ~0.2-0.3 ms).
    struct ScanStats
    {
        uint64_t calls = 0, cols = 0, fastCalls = 0, must = 0;
        double msMax = 0, fastMsSum = 0, fastMsMax = 0;
    } ss;
    std::vector<collision::PlanItem> plan;
    std::vector<proto::ColTri> col;
    auto scan = [&](float cx, float cy, float cz) {
        LARGE_INTEGER f, q0;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&q0);
        auto us = [&] {
            LARGE_INTEGER q;
            QueryPerformanceCounter(&q);
            return static_cast<double>(q.QuadPart - q0.QuadPart) * 1e6 / static_cast<double>(f.QuadPart);
        };
        const double mx = cx, my = cz, mz = -cy;
        const uint64_t now = GetTickCount64();
        collision::Motion mo;
        if (collision::GetMotion(mo, now) && std::fabs(mo.x - mx) + std::fabs(mo.z - mz) > 6.0)
            mo.valid = false;
        if (!mo.valid)
            mo.x = mx, mo.y = my, mo.z = mz, mo.vx = mo.vy = mo.vz = 0;
        const double speed = mo.valid ? mo.Speed() : 0.0;
        collision::Motion focus; // an ender pearl in flight (as collision_physx.cpp)
        const bool haveFocus = !legacy && collision::GetFocus(focus, now);
        const double budget = 1000.0 + 1500.0 * (haveFocus ? 1.0 : std::clamp((speed - 4.0) / 8.0, 0.0, 1.0));
        collision::PlanSettings ps;
        collision::PlanScan(mx, my, mz, mo, ps, now, plan, haveFocus ? &focus : nullptr);
        const double planUs = us();
        double extractUs = 0, setUs = 0;
        int done = 0;
        for (const auto& c : plan)
        {
            const bool must = c.must;
            if (done > 0 && us() + 225.0 > (must ? 5000.0 : budget))
                break;
            if (must && us() + 225.0 > budget)
                ++ss.must;
            const double c0 = us();
            bool terrain = false;
            extract(c.rx, c.rz, c.ryLo, c.ryHi, col, terrain);
            extractUs += us() - c0;
            // WC_STREAM_RADIUS: The Witcher only has its world loaded around Geralt (he follows Steve);
            // further out a scan finds nothing yet (in game: elytra flight outran it).
            static const double streamR = std::getenv("WC_STREAM_RADIUS") ? std::atof(std::getenv("WC_STREAM_RADIUS")) : 0.0;
            if (streamR > 0.0 && std::hypot(c.rx * 8.0 + 4.0 - mx, c.rz * 8.0 + 4.0 - mz) > streamR)
            {
                col.clear();
                terrain = false;
            }
            static const double costUs = std::getenv("WC_SCAN_COST_US") ? std::atof(std::getenv("WC_SCAN_COST_US")) : 300.0; // heavy towns: ~1000-1400
            while (us() - c0 < costUs)
            {
            }
            const double s0 = us();
            collision::SetColumn(c.rx, c.rz, c.ryLo, c.ryHi, col.data(), col.size(), now);
            collision::MarkScanned(c.rx, c.rz, c.ryLo, c.ryHi, terrain, now);
            setUs += us() - s0;
            ++done;
        }
        const double ms = us() / 1000.0;
        if (ms > 8.0)
            WLOG("flight test: slow scan call %.1f ms (%d columns, plan %zu, first must %d; plan %.1f ms, extract %.1f ms, store %.1f ms)", ms, done,
                 plan.size(), plan.empty() ? 0 : plan[0].must, planUs / 1000.0, extractUs / 1000.0, setUs / 1000.0);
        ++ss.calls;
        ss.cols += done;
        ss.msMax = std::max(ss.msMax, ms);
        if (speed >= 8.0)
        {
            ++ss.fastCalls;
            ss.fastMsSum += ms;
            ss.fastMsMax = std::max(ss.fastMsMax, ms);
        }
    };

    control::SetMcMode(true);
    if (!host::Start())
        return 1;
    host::SetWorld("fake\\flight_test.w2w");
    // WC_VIEWPORT=2560x1440: Minecraft renders the overlay at a real game size (1440p), so
    // its frame capture / readback costs are the real ones.
    if (const char* vp = std::getenv("WC_VIEWPORT"))
    {
        unsigned vw = 0, vh = 0;
        if (std::sscanf(vp, "%ux%u", &vw, &vh) == 2)
            host::SetViewport(vw, vh);
    }
    collision::SetSourceActive(true);

    enum Phase
    {
        WaitDriven,
        Walk,
        Setup,
        Run,
        Teleport,
        Fall,
        PearlSetup,
        PearlThrow,
        PearlChain,
        SwimTeleport,
        Swim,
        ClimbTeleport,
        Climb,
        Resume,
        Done,
        Failed
    };
    const char* names[] = {"WaitDriven", "Walk",  "Setup",         "Run",   "Teleport", "Fall", "PearlSetup", "PearlThrow", "PearlChain",
                           "SwimTeleport", "Swim", "ClimbTeleport", "Climb", "Resume", "Done",     "Failed"};
    Phase phase = WaitDriven;
    double phaseT = 0;
    std::string fails;
    auto fail = [&](const char* what) {
        WLOG("flight test: FAIL: %s", what);
        fails += what;
        fails += "; ";
    };
    float geraltX = sx, geraltY = sy, geraltZ = G;
    bool followMc = true, sendTeleport = false;
    int scanIndex = 0;
    const double t0 = GetTickCount64() / 1000.0;
    double lastStep = -1, lastPrint = -1;
    double maxY = -1e9, minZ = 1e9, maxSpeed = 0, maxFall = 0, lastZ = 0, lastT = 0, landT = -1;
    double runSpeed = 0, fallSpeed = 0, hostSendMax = 0;
    bool runOk = false, fallOk = false, sawGate = false, walkOk = legacy, walkGate = false, pearlOk = legacy;
    double pearlX0 = 0, pearlY0 = 0, pearlBest = 0;
    int pearlClick = 0;
    bool swimOk = legacy, climbOk = legacy;
    // PEARL CHAIN: 4 more pearls thrown one after the other into new ground, as a
    // player would in a city: each must land on the terrain; reports how long Minecraft held them.
    bool chainOk = legacy, chainWaiting = false, chainReleased = false;
    int chainDone = 0, chainLanded = 0;
    double chainX0 = 0, chainY0 = 0, chainThrowT = 0, chainStartT = 0, chainMoved = 0;
    uint64_t chainHolds0 = 0, chainPearls0 = 0;
    double chainHeldSum0 = 0;
    double swimTop = -1e9, climbTop = -1e9, climbJumpT = -1;
    // RESUME: The Witcher pauses its scripts (a menu, the map) for 1.5 s and resumes at
    // the same spot: the hold gate must let Steve go quickly (in game every resume took 1.5 s: it
    // waited for the periodic rescan of the columns around him).
    bool resumeOk = legacy;
    double pauseUntil = -1, resumeHoldMs = -1;
    uint64_t pearlRescues0 = 0;
    double walkSpeed = 0, walkStartX = 0, walkStartY = 0, lastJump = -1;
    bool jumpHeld = false;
    uint64_t gateHolds0 = 0;
    uint64_t rescues0 = 0, guard0 = 0;

    while (!g_wallsQuit && phase != Done && phase != Failed)
    {
        const double t = GetTickCount64() / 1000.0 - t0;
        if (t > timeout)
        {
            WLOG("flight test: TIMEOUT in phase %s", names[phase]);
            fail("timeout");
            phase = Failed;
            break;
        }
        const control::Frame fr = control::OnFrame(0, 0.0f, 0.0f);
        const auto st = host::GetStatus();
        const bool inWorld = st.mcAlive && st.haveMcState && (st.mcFlags & proto::kMcInWorld);
        const double mcX = st.mcX, mcY = -st.mcZ, mcZ = st.mcY; // MC feet in W3 coords
        maxSpeed = std::max(maxSpeed, st.speed);
        hostSendMax = std::max(hostSendMax, st.sendMsMax);
        if (st.gateHolding && phase == Teleport)
            sawGate = true;

        if (t - lastStep >= 0.033 && t >= pauseUntil) // RESUME: no script steps while paused
        {
            lastStep = t;
            if (followMc && fr.engaged && fr.eyeValid && fr.arrived)
            {
                geraltX = static_cast<float>(fr.feet[0]);
                geraltY = static_cast<float>(fr.feet[1]);
                geraltZ = static_cast<float>(fr.feet[2]);
            }
            float cx = geraltX, cy = geraltY, cz = geraltZ;
            host::SampleCentre(cx, cy, cz);
            std::string batch;
            const int bx = static_cast<int>(std::floor(cx)), by = static_cast<int>(std::floor(cy));
            for (int n = 0; n < 96; ++n) // as witchercraft.ws: 96 traces per step
            {
                const int idx = scanIndex++ % (33 * 33);
                const int wx = bx + idx % 33 - 16, wy = by + idx / 33 - 16;
                static const double streamR = std::getenv("WC_STREAM_RADIUS") ? std::atof(std::getenv("WC_STREAM_RADIUS")) : 0.0;
                const bool streamed = streamR <= 0.0 || std::hypot(wx - geraltX, wy - geraltY) <= streamR;
                const float g = streamed ? groundAt(wx, wy, cz) : NAN;
                const float w = legacy || !streamed ? NAN : waterAt(wx, wy);
                char item[64];
                if (std::isnan(g))
                    std::snprintf(item, sizeof(item), "%d,%d,n;", wx, wy);
                else if (!std::isnan(w))
                    std::snprintf(item, sizeof(item), "%d,%d,%.3f,%.3f;", wx, wy, g, w); // as witchercraft.ws (M7)
                else
                    std::snprintf(item, sizeof(item), "%d,%d,%.3f;", wx, wy, g);
                batch += item;
            }
            host::PushHeights(batch);
            float scx = cx, scy = cy, scz = cz;
            host::ScanCentre(scx, scy, scz);
            scan(scx, scy, scz); // the script calls WCraft_ColScan every step
            uint32_t flags = host::kTickInGame;
            if (sendTeleport)
            {
                flags |= host::kTickTeleport;
                sendTeleport = false;
            }
            host::Tick(geraltX, geraltY, geraltZ, 180.0f - st.mcYaw, -st.mcPitch, flags);
        }

        switch (phase)
        {
        case WaitDriven:
            if (st.driven && inWorld && st.teleportAck == st.teleportSeq && !st.gateHolding && fr.eyeValid)
            {
                WLOG("flight test: Minecraft drives (t=%.1f s) at W3 (%.2f, %.2f, %.2f)", t, mcX, mcY, mcZ);
                if (legacy)
                {
                    buildmode::QueueCommand("gamemode creative");
                    buildmode::QueueCommand("attribute @s minecraft:movement_speed base set 1.0");
                    phase = Setup;
                }
                else
                {
                    collision::FlightConfig wf = collision::GetFlightConfig();
                    wf.guardMinSpeed = 3.0f;
                    collision::SetFlightConfig(wf);
                    buildmode::QueueCommand("gamemode survival");
                    phase = Walk;
                }
                phaseT = t;
            }
            break;
        case Walk:
            if (st.gateHolding && t - phaseT > 2.0)
                walkGate = true;
            if (t - phaseT > 1.5 && t - phaseT < 1.6 && guard0 == 0 && gateHolds0 == 0)
            {
                walkStartX = mcX, walkStartY = mcY;
                guard0 = st.flightHolds + 1; // +1: "started" marker, taken off below
                gateHolds0 = st.gateHolds + 1;
                WLOG("flight test: WALK: sprint + jump at survival speed for 12 s (guard floor 3 blocks/s) from (%.2f, %.2f, %.2f)", mcX, mcY, mcZ);
                buildmode::QueueEvent({proto::kInKey, 224, 1, 0, 0}); // LCtrl: sprint
                buildmode::QueueEvent({proto::kInKey, 26, 1, 0, 0});  // W
            }
            if (guard0 > 0 && t - phaseT > 2.0)
            {
                walkSpeed = std::max(walkSpeed, st.speed);
                if (t - lastJump > 0.9)
                {
                    lastJump = t;
                    jumpHeld = true;
                    buildmode::QueueEvent({proto::kInKey, 44, 1, 0, 0}); // Space
                }
                else if (t - lastJump > 0.3 && jumpHeld)
                {
                    jumpHeld = false; // one release per jump (the loop runs every ~8 ms)
                    buildmode::QueueEvent({proto::kInKey, 44, 0, 0, 0});
                }
            }
            if (guard0 > 0 && t - phaseT > 13.5)
            {
                buildmode::QueueEvent({proto::kInKey, 44, 0, 0, 0});
                buildmode::QueueEvent({proto::kInKey, 26, 0, 0, 0});
                buildmode::QueueEvent({proto::kInKey, 224, 0, 0, 0});
                const uint64_t holds = st.flightHolds - (guard0 - 1), gates = st.gateHolds - (gateHolds0 - 1);
                const double dist = std::hypot(mcX - walkStartX, mcY - walkStartY);
                walkOk = holds == 0 && gates == 0 && !walkGate && dist > 40.0;
                WLOG("flight test: WALK: %.1f m in 12 s, top speed %.1f blocks/s, flight guard holds %llu, hold-gate holds %llu -> %s", dist, walkSpeed,
                     static_cast<unsigned long long>(holds), static_cast<unsigned long long>(gates), walkOk ? "NO FREEZES (OK)" : "BAD");
                if (!walkOk)
                    fail(dist <= 40.0 ? "walk: did not move" : "walk: Steve was frozen while walking/jumping");
                guard0 = 0;
                collision::SetFlightConfig(fc);
                buildmode::QueueCommand("gamemode creative");
                buildmode::QueueCommand("attribute @s minecraft:movement_speed base set 1.0");
                phase = Setup;
                phaseT = t;
            }
            break;
        case Setup:
            if (t - phaseT > 3.0 && buildmode::CommandsPending() == 0 && !(st.mcFlags & proto::kMcScreenOpen))
            {
                maxY = mcY;
                minZ = mcZ;
                guard0 = st.flightHolds;
                WLOG("flight test: RUN: W for 6 s at ~10x walking speed from (%.2f, %.2f, %.2f) towards the wall at y %.2f (150 m)", mcX, mcY, mcZ,
                     wallY);
                buildmode::QueueEvent({proto::kInKey, 26, 1, 0, 0});
                phase = Run;
                phaseT = t;
            }
            break;
        case Run:
            maxY = std::max(maxY, mcY);
            minZ = std::min(minZ, mcZ);
            runSpeed = std::max(runSpeed, st.speed);
            if (t - phaseT > 6.0)
            {
                buildmode::QueueEvent({proto::kInKey, 26, 0, 0, 0});
                runOk = maxY < wallY && mcY > wallY - 1.0 && minZ > G - 0.2;
                WLOG("flight test: RUN: top speed %.1f blocks/s, max y %.2f, now (%.2f, %.2f, %.2f), lowest z %.2f; wall face at %.2f -> %s "
                     "(flight guard holds %llu)",
                     runSpeed, maxY, mcX, mcY, mcZ, minZ, wallY, runOk ? "STOPPED AT THE WALL (OK)" : "BAD",
                     static_cast<unsigned long long>(st.flightHolds - guard0));
                if (!runOk)
                    fail(maxY >= wallY ? "ran through the wall" : (minZ <= G - 0.2 ? "fell through the ground" : "did not reach the wall"));
                followMc = false;
                geraltX = towerX;
                geraltY = sy;
                geraltZ = towerTop;
                sendTeleport = true;
                phase = Teleport;
                phaseT = t;
            }
            break;
        case Teleport:
            if (t - phaseT > 1.0 && inWorld && st.teleportAck == st.teleportSeq && !st.gateHolding && std::fabs(mcX - towerX) < 0.8 &&
                std::fabs(mcZ - towerTop) < 0.6 && fr.arrived)
            {
                WLOG("flight test: on the tower top (t=%.1f s) (%.2f, %.2f, %.2f); FALL: W, run off the edge, fall %.0f m", t, mcX, mcY, mcZ,
                     towerTop - G);
                followMc = true;
                rescues0 = st.rescues;
                guard0 = st.flightHolds;
                minZ = mcZ;
                lastZ = mcZ;
                lastT = t;
                landT = -1;
                buildmode::QueueEvent({proto::kInKey, 26, 1, 0, 0});
                phase = Fall;
                phaseT = t;
            }
            else if (t - phaseT > 40.0)
            {
                fail("teleport to the tower");
                phase = Failed;
            }
            break;
        case Fall:
            if (inWorld) // a one-iteration "not in world" blip reads (0, 0, 0) (known, Minecraft-side)
                minZ = std::min(minZ, mcZ);
            if (t - lastT > 0.05)
            {
                maxFall = std::max(maxFall, (lastZ - mcZ) / (t - lastT));
                lastZ = mcZ, lastT = t;
            }
            fallSpeed = std::max(fallSpeed, st.speed);
            if (t - phaseT > 0.6 && t - phaseT < 0.7)
                buildmode::QueueEvent({proto::kInKey, 26, 0, 0, 0});
            if (landT < 0 && mcZ < G + 0.3 && (st.mcFlags & proto::kMcOnGround))
                landT = t;
            if ((landT > 0 && t - landT > 2.0) || t - phaseT > 25.0 || st.rescues != rescues0)
            {
                fallOk = landT > 0 && minZ > G - 0.3 && std::fabs(mcZ - G) < 0.3 && st.rescues == rescues0;
                WLOG("flight test: FALL: top speed %.1f blocks/s (falling %.1f), lowest z %.2f (ground %.2f), now (%.2f, %.2f, %.2f), landed %s, "
                     "safety-net rescues %llu, flight guard holds %llu -> %s",
                     fallSpeed, maxFall, minZ, G, mcX, mcY, mcZ, landT > 0 ? "yes" : "NO", static_cast<unsigned long long>(st.rescues - rescues0),
                     static_cast<unsigned long long>(st.flightHolds - guard0), fallOk ? "LANDED ON THE TERRAIN (OK)" : "BAD");
                if (!fallOk)
                    fail(st.rescues != rescues0 || minZ <= G - 0.3 ? "fell through the terrain" : "did not land");
                if (legacy)
                    phase = Done;
                else
                {
                    buildmode::QueueCommand("item replace entity @s weapon.mainhand with minecraft:ender_pearl 16");
                    phase = PearlSetup;
                }
                phaseT = t;
            }
            break;
        case PearlSetup:
            if (t - phaseT > 2.0 && buildmode::CommandsPending() == 0 && !(st.mcFlags & proto::kMcScreenOpen) && (st.mcFlags & proto::kMcOnGround))
            {
                float yaw = 0, pitch = 0;
                control::GetLook(yaw, pitch);
                control::SetLook(yaw, -30.0f);
                pearlX0 = mcX, pearlY0 = mcY;
                pearlRescues0 = st.rescues;
                WLOG("flight test: PEARL: throwing an ender pearl 30 degrees up from (%.2f, %.2f, %.2f), yaw %.1f", mcX, mcY, mcZ, yaw);
                phase = PearlThrow;
                phaseT = t;
            }
            break;
        case PearlThrow:
            // One press and one release (an 8 ms loop would queue several inside a 50 ms window).
            if (t - phaseT > 0.5 && pearlClick == 0)
                buildmode::QueueEvent({proto::kInMouseButton, 3, 1, 0, 0}), pearlClick = 1;
            if (t - phaseT > 0.7 && pearlClick == 1)
                buildmode::QueueEvent({proto::kInMouseButton, 3, 0, 0, 0}), pearlClick = 2;
            {
                const double moved = std::hypot(mcX - pearlX0, mcY - pearlY0);
                pearlBest = std::max(pearlBest, moved);
                const bool landed = moved > 20.0 && std::fabs(mcZ - G) < 0.3 && (st.mcFlags & proto::kMcOnGround);
                if ((landed && t - phaseT > 2.0) || t - phaseT > 12.0 || st.rescues != pearlRescues0)
                {
                    pearlOk = landed && st.rescues == pearlRescues0;
                    WLOG("flight test: PEARL: Steve moved %.1f m (best %.1f), now (%.2f, %.2f, %.2f), ground %.2f, rescues %llu -> %s", moved, pearlBest, mcX,
                         mcY, mcZ, G, static_cast<unsigned long long>(st.rescues - pearlRescues0),
                         pearlOk ? "TELEPORTED ONTO THE TERRAIN (OK)" : "BAD");
                    if (!pearlOk)
                        fail(moved <= 20.0 ? "pearl: no teleport" : "pearl: not on the terrain");
                    // PEARL CHAIN: east (MC yaw -90 = +x: open terrain past the tower), 20 degrees up.
                    control::SetLook(-90.0f, -20.0f);
                    chainHolds0 = st.pearlHolds, chainHeldSum0 = st.pearlHeldMsSum, chainPearls0 = st.pearls;
                    chainStartT = t;
                    WLOG("flight test: PEARL CHAIN: 4 pearls east, one after the other, from (%.2f, %.2f, %.2f)", mcX, mcY, mcZ);
                    phase = PearlChain;
                    phaseT = t;
                }
            }
            break;
        case PearlChain:
            if (!chainWaiting && chainDone < 4 && t - phaseT > 1.2 && (st.mcFlags & proto::kMcOnGround) && t - chainThrowT > 1.3)
            {
                chainX0 = mcX, chainY0 = mcY;
                chainThrowT = t;
                chainWaiting = true;
                chainReleased = false;
                buildmode::QueueEvent({proto::kInMouseButton, 3, 1, 0, 0});
            }
            if (chainWaiting && t - chainThrowT > 0.15 && !chainReleased)
                buildmode::QueueEvent({proto::kInMouseButton, 3, 0, 0, 0}), chainReleased = true;
            if (chainWaiting && t - chainThrowT > 0.3)
            {
                const double moved = std::hypot(mcX - chainX0, mcY - chainY0);
                const bool landed = moved > 15.0 && std::fabs(mcZ - G) < 0.3 && (st.mcFlags & proto::kMcOnGround);
                if (landed || t - chainThrowT > 10.0 || st.rescues != pearlRescues0)
                {
                    WLOG("flight test: PEARL CHAIN %d: %s, moved %.1f m in %.2f s, now (%.2f, %.2f, %.2f)", chainDone + 1, landed ? "landed" : "BAD", moved,
                         t - chainThrowT, mcX, mcY, mcZ);
                    chainLanded += landed ? 1 : 0;
                    chainMoved += moved;
                    ++chainDone;
                    chainWaiting = false;
                    chainThrowT = t; // the next throw waits for MC's pearl cooldown
                }
            }
            if (chainDone >= 4 || t - phaseT > 60.0)
            {
                const uint64_t holds = st.pearlHolds - chainHolds0;
                const double held = st.pearlHeldMsSum - chainHeldSum0;
                chainOk = chainLanded == 4 && st.rescues == pearlRescues0;
                WLOG("flight test: PEARL CHAIN: %d/4 landed on the terrain, %.0f m in %.1f s; pearls %llu, held in the air %llu times, %.0f ms in "
                     "total (longest so far %.0f ms) -> %s",
                     chainLanded, chainMoved, t - chainStartT, static_cast<unsigned long long>(st.pearls - chainPearls0),
                     static_cast<unsigned long long>(holds), held, st.pearlHeldMsMax, chainOk ? "OK" : "BAD");
                if (!chainOk)
                    fail("pearl chain: a pearl did not land on the terrain");
                {
                    // M7 SWIM: into the flooded field (water 3 m deep).
                    buildmode::QueueCommand("gamemode survival");
                    buildmode::QueueCommand("attribute @s minecraft:movement_speed base set 0.1");
                    control::SetLook(0.0f, 0.0f);
                    followMc = false;
                    geraltX = (poolX0 + poolX1) * 0.5f;
                    geraltY = (poolY0 + poolY1) * 0.5f;
                    geraltZ = G;
                    sendTeleport = true;
                    phase = SwimTeleport;
                    phaseT = t;
                }
            }
            break;
        case SwimTeleport:
            if (t - phaseT > 3.0 && inWorld && st.teleportAck == st.teleportSeq && !st.gateHolding && std::fabs(mcX - geraltX) < 1.5 &&
                std::fabs(mcY - geraltY) < 1.5 && buildmode::CommandsPending() == 0 && !(st.mcFlags & proto::kMcScreenOpen))
            {
                followMc = true;
                swimTop = mcZ;
                WLOG("flight test: SWIM: in the flooded field at (%.2f, %.2f, %.2f), ground %.1f, water surface %.1f; holding Space for 4 s", mcX, mcY, mcZ,
                     G, poolW);
                buildmode::QueueEvent({proto::kInKey, 44, 1, 0, 0});
                phase = Swim;
                phaseT = t;
            }
            else if (t - phaseT > 40.0)
            {
                fail("swim: teleport to the water");
                phase = Failed;
            }
            break;
        case Swim:
            if (t - phaseT > 1.0)
                swimTop = std::max(swimTop, mcZ);
            if (t - phaseT > 4.0)
            {
                buildmode::QueueEvent({proto::kInKey, 44, 0, 0, 0});
                // On land, Space only jumps 1.25 m; in water Steve swims up to the surface.
                swimOk = swimTop > G + 1.6 && swimTop < poolW + 1.0;
                WLOG("flight test: SWIM: highest feet %.2f (ground %.1f, surface %.1f), now %.2f -> %s", swimTop, G, poolW, mcZ,
                     swimOk ? "SWAM UP TO THE SURFACE (OK)" : "BAD (no water in Minecraft)");
                if (!swimOk)
                    fail("swim: Minecraft has no water there");
                // M7 CLIMB: 3 m west of the crate's west face, facing east (+x).
                followMc = false;
                geraltX = sx + 17.0f;
                geraltY = sy + 32.0f;
                geraltZ = G;
                sendTeleport = true;
                phase = ClimbTeleport;
                phaseT = t;
            }
            break;
        case ClimbTeleport:
            if (t - phaseT > 3.0 && inWorld && st.teleportAck == st.teleportSeq && !st.gateHolding && std::fabs(mcX - geraltX) < 1.5 &&
                std::fabs(mcY - geraltY) < 1.5 && std::fabs(mcZ - G) < 0.3)
            {
                followMc = true;
                control::SetLook(-90.0f, 0.0f); // MC yaw -90 = +x
                climbTop = mcZ;
                WLOG("flight test: CLIMB: at (%.2f, %.2f, %.2f), a %.1f m crate 3 m east; W + Space", mcX, mcY, mcZ, 2.5);
                buildmode::QueueEvent({proto::kInKey, 26, 1, 0, 0});
                phase = Climb;
                phaseT = t;
            }
            else if (t - phaseT > 40.0)
            {
                fail("climb: teleport");
                phase = Failed;
            }
            break;
        case Climb:
            climbTop = std::max(climbTop, mcZ);
            // One jump about a block before the crate's face (as a player would), then stop on top.
            if (climbJumpT < 0 && mcX > sx + 20.0f - 1.3f)
            {
                climbJumpT = t;
                buildmode::QueueEvent({proto::kInKey, 44, 1, 0, 0});
            }
            if (climbJumpT > 0 && t - climbJumpT > 0.3 && t - climbJumpT < 0.35)
                buildmode::QueueEvent({proto::kInKey, 44, 0, 0, 0});
            if (climbJumpT > 0 && t - climbJumpT > 1.2 && t - climbJumpT < 1.25)
                buildmode::QueueEvent({proto::kInKey, 26, 0, 0, 0});
            if (t - phaseT > 4.0)
            {
                climbOk = std::fabs(mcZ - (G + 2.5f)) < 0.3 && mcX > sx + 20.0f;
                WLOG("flight test: CLIMB: now (%.2f, %.2f, %.2f), highest %.2f; crate top %.2f -> %s", mcX, mcY, mcZ, climbTop, G + 2.5f,
                     climbOk ? "ON THE CRATE (OK)" : "BAD (still below)");
                if (!climbOk)
                    fail("climb: not on the crate");
                pauseUntil = t + 1.5;
                WLOG("flight test: RESUME: The Witcher pauses its scripts for 1.5 s (a menu) at (%.2f, %.2f, %.2f)", mcX, mcY, mcZ);
                phase = Resume;
            }
            break;
        case Resume:
            if (t > pauseUntil + 0.1 && inWorld && st.teleportAck == st.teleportSeq && !st.gateHolding && fr.arrived)
            {
                resumeHoldMs = (t - pauseUntil) * 1000.0;
                resumeOk = resumeHoldMs < 1000.0;
                WLOG("flight test: RESUME: Steve free again %.0f ms after the scripts resumed -> %s", resumeHoldMs,
                     resumeOk ? "OK" : "BAD (held as long as a periodic rescan)");
                if (!resumeOk)
                    fail("resume: the hold gate held Steve too long after a pause");
                phase = Done;
            }
            else if (t - pauseUntil > 20.0)
            {
                fail("resume: never released");
                phase = Failed;
            }
            break;
        default:
            break;
        }
        if (t - lastPrint >= 1.0)
        {
            lastPrint = t;
            WLOG("t=%5.1fs %s | MC (%.2f, %.2f, %.2f) v=%.1f | %s | %s", t, names[phase], mcX, mcY, mcZ, st.speed, host::StatusLine().c_str(),
                 collision::StoreStatus().c_str());
        }
        // ~8 ms per loop = one "game frame": Minecraft is paced at ~120 frames/s as in game (a plain
        // Sleep(8) is 15.6 ms on a hidden console: 64 frames/s, where frame drops barely show).
        if (frameTimer)
        {
            LARGE_INTEGER due;
            due.QuadPart = -80000; // 8 ms
            SetWaitableTimer(frameTimer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(frameTimer, 50);
        }
        else
            Sleep(8);
    }
    buildmode::QueueEvent({proto::kInKey, 26, 0, 0, 0});
    buildmode::QueueCommand("attribute @s minecraft:movement_speed base set 0.1");
    for (int i = 0; i < 60; ++i)
    {
        control::OnFrame(0, 0.0f, 0.0f);
        host::Tick(geraltX, geraltY, geraltZ, 0.0f, 0.0f, host::kTickInGame);
        Sleep(33);
    }
    const auto st = host::GetStatus();
    const bool pass = phase == Done && fails.empty() && runOk && fallOk && walkOk && pearlOk && chainOk && swimOk && climbOk && resumeOk;
    WLOG("flight test: scan calls %llu, columns %llu (%.1f per call), max call %.2f ms; while fast: %llu calls avg %.2f max %.2f ms, %llu columns "
         "needed within 0.5 s went over the normal budget (emulated 0.3 ms per column)",
         static_cast<unsigned long long>(ss.calls), static_cast<unsigned long long>(ss.cols), ss.calls ? double(ss.cols) / ss.calls : 0.0, ss.msMax,
         static_cast<unsigned long long>(ss.fastCalls), ss.fastCalls ? ss.fastMsSum / ss.fastCalls : 0.0, ss.fastMsMax,
         static_cast<unsigned long long>(ss.must));
    WLOG("flight test: host: top speed %.1f blocks/s, columns sent %llu (ahead %llu, early %llu), guard holds %llu skips %llu, longest column "
         "building per iteration %.1f ms",
         maxSpeed, static_cast<unsigned long long>(st.columnsSent), static_cast<unsigned long long>(st.aheadSent),
         static_cast<unsigned long long>(st.partialSent), static_cast<unsigned long long>(st.flightHolds), static_cast<unsigned long long>(st.guardSkips),
         hostSendMax);
    WLOG("flight test%s: %s (walk %d run %d fall %d pearl %d chain %d swim %d climb %d resume %d gate %d) %s; %s", legacy ? " (LEGACY)" : "",
         pass ? "PASS" : "FAIL", walkOk, runOk, fallOk, pearlOk, chainOk, swimOk, climbOk, resumeOk, sawGate,
         fails.c_str(),
         host::StatusLine().c_str());
    host::Stop();
    return pass ? 0 : 2;
}
