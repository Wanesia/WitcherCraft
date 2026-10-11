// ---- WitcherCraft TNT offline test: chain reactions on the Witcher's ground ---------------------
//
//   fake_host.exe tnt [timeoutSeconds=240]
//
// Flat fake ground (script heights) plus two flat "street" slabs (PhysX-style triangles, not
// terrain: a building surface that can't be dug, SkyDig KEEP), Minecraft mode, creative. Three
// rows of 5 TNT, each sunk into the ground the way a block placed on uneven Witcher ground sits
// (SkyRay.placementCell: up to 0.6 m sunk in). Each row is lit at one end (a redstone block next
// to it, which primes the end TNT like flint and steel); then all 5 must explode (kEvExplosion
// events, radius 4, near the row) and no TNT block may be left (/fill ... replace tnt must say
// "No blocks were filled").
//   A. STREET: the row sunk 0.4 m into a street slab, World destruction on. A bandit stand-in
//      3 m away must be hit by the blasts (kEvHitActor).
//   B. GROUND: the row sunk 0.45 m into the Witcher's terrain, World destruction on.
//   C. STREET, World destruction off (player tag witchercraft_test_nodig: SkyDigBlast acts as if
//      the pause menu's "World destruction" were off, without touching the player's config file).
// Exit code 0 = PASS.
#include "buildmode.hpp"
#include "collision.hpp"
#include "combat.hpp"
#include "control.hpp"
#include "log.hpp"
#include "skycraft_host.hpp"
#include "skycraft_protocol.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
namespace proto = skycraft::proto;

volatile bool g_tntQuit = false;
BOOL WINAPI OnCtrlTnt(DWORD)
{
    g_tntQuit = true;
    return TRUE;
}

// WC_MC_LOG points at another Minecraft's log (e.g. the release's Prism instance).
const char* const kMcLog = std::getenv("WC_MC_LOG") ? std::getenv("WC_MC_LOG") : "minecraft\\run\\logs\\latest.log";

size_t McLogSize()
{
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExA(kMcLog, GetFileExInfoStandard, &a))
        return 0;
    return static_cast<size_t>(a.nFileSizeLow);
}

bool McLogHas(const char* what, size_t from)
{
    HANDLE h = CreateFileA(kMcLog, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    std::string s;
    char buf[65536];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0)
        s.append(buf, n);
    CloseHandle(h);
    return s.size() > from && s.find(what, from) != std::string::npos;
}

int FloorDiv8(int v)
{
    return v >= 0 ? v / 8 : -((-v + 7) / 8);
}

struct BoxMc // axis-aligned, Minecraft coords
{
    float x0, y0, z0, x1, y1, z1;
};

// The 12 triangles of a box (MC coords), flags 0: a PhysX mesh, not terrain, not diggable.
void BoxTris(const BoxMc& b, std::vector<proto::ColTri>& out)
{
    float c[8][3];
    for (int i = 0; i < 8; ++i)
    {
        c[i][0] = (i & 1) ? b.x1 : b.x0;
        c[i][1] = (i & 2) ? b.y1 : b.y0;
        c[i][2] = (i & 4) ? b.z1 : b.z0;
    }
    // Outward-facing quads (counter-clockwise seen from outside).
    static const int f[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
    for (const auto& q : f)
    {
        const int tri[2][3] = {{q[0], q[1], q[2]}, {q[0], q[2], q[3]}};
        for (const auto& t : tri)
        {
            proto::ColTri ct{};
            for (int k = 0; k < 3; ++k)
                for (int a = 0; a < 3; ++a)
                    ct.v[k * 3 + a] = c[t[k]][a];
            ct.flags = 0;
            out.push_back(ct);
        }
    }
}

struct Row
{
    const char* name;
    int x0, y, z; // MC: TNT at x0..x0+4, y, z; the redstone block at x0-1 (or x0+5 if fromEast)
    bool fromEast;
    bool noDig;   // World destruction off
    bool bandit;  // a bandit stand-in nearby must be hit
};
} // namespace

int RunTntTest(int timeout)
{
    using namespace wc;
    SetConsoleCtrlHandler(OnCtrlTnt, TRUE);
    const float G = 64.0f;
    const uint64_t seed = (GetTickCount64() / 1000) % 200;
    const float sx = -70000.0f - static_cast<float>(seed) * 300.0f + 0.5f, sy = 900.5f;
    const int mx0 = static_cast<int>(std::floor(sx)), mz0 = static_cast<int>(std::floor(-sy)); // the player's MC cell
    const int by = static_cast<int>(G);

    // A: street slab east of the player; C: another one further north (MC -z).
    const float streetTop = G + 0.4f;
    std::vector<BoxMc> slabs = {
        {static_cast<float>(mx0 + 2), G - 0.3f, static_cast<float>(mz0 - 3), static_cast<float>(mx0 + 14), streetTop, static_cast<float>(mz0 + 4)},
        {static_cast<float>(mx0 + 2), G - 0.3f, static_cast<float>(mz0 - 15), static_cast<float>(mx0 + 14), streetTop, static_cast<float>(mz0 - 8)},
    };
    std::vector<proto::ColTri> all;
    for (const auto& b : slabs)
        BoxTris(b, all);
    // B: a terrain patch west of the player, 0.45 m higher (W3 lattice: x, y = -MC z).
    const float patchH = G + 0.45f;
    auto groundAt = [&](int wx, int wy) {
        const int mz = -wy;
        if (wx >= mx0 - 13 && wx <= mx0 - 2 && mz >= mz0 - 3 && mz <= mz0 + 4)
            return patchH;
        return G;
    };
    const Row rows[] = {
        {"A STREET (destruction on)", mx0 + 5, by, mz0, false, false, true},
        {"B GROUND (destruction on)", mx0 - 9, by, mz0, true, false, false},
        {"C STREET (destruction off)", mx0 + 5, by, mz0 - 11, false, true, false},
    };
    const int kRows = 3;
    WLOG("tnt test: start W3 (%.1f, %.1f, %.1f), MC cell %d %d %d; street top %.2f, ground patch %.2f; %zu triangles; timeout %d s", sx, sy, G,
         mx0, by, mz0, streetTop, patchH, all.size(), timeout);

    // The native scanner's job (as fake_host_walls.cpp): every region column near the centre gets
    // the triangles touching it.
    auto scan = [&](float cx, float cy, float cz) {
        const double mx = cx, my = cz, mz = -cy;
        const int prx = FloorDiv8(static_cast<int>(std::floor(mx))), prz = FloorDiv8(static_cast<int>(std::floor(mz)));
        const int fy = static_cast<int>(std::floor(my));
        const int ryLo = FloorDiv8(fy - 12), ryHi = FloorDiv8(fy + 20);
        std::vector<proto::ColTri> col;
        for (int rx = prx - 3; rx <= prx + 3; ++rx)
            for (int rz = prz - 3; rz <= prz + 3; ++rz)
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
    host::SetWorld("fake\\tnt_test.w2w");
    collision::SetSourceActive(true);

    enum Phase
    {
        WaitDriven,
        Setup,
        Place,
        Ignite,
        Boom,
        Check,
        Done,
        Failed
    };
    const char* names[] = {"WaitDriven", "Setup", "Place", "Ignite", "Boom", "Check", "Done", "Failed"};
    Phase phase = WaitDriven;
    double phaseT = 0;
    std::string fails;
    auto fail = [&](const std::string& what) {
        WLOG("tnt test: FAIL: %s", what.c_str());
        fails += what;
        fails += "; ";
    };
    float geraltX = sx, geraltY = sy, geraltZ = G;
    int scanIndex = 0;
    const double t0 = GetTickCount64() / 1000.0;
    double lastStep = -1, lastPrint = -1, lastScan = -1, lastBoomT = 0;
    int row = 0, booms = 0, banditHits = 0, totalBooms = 0;
    bool rowOk[kRows] = {};
    size_t log0 = 0;
    const int32_t banditId = -3456789;
    auto cmd = [&](const char* fmt, auto... args) {
        char c[192];
        std::snprintf(c, sizeof(c), fmt, args...);
        buildmode::QueueCommand(c);
        WLOG("tnt test: /%s", c);
    };

    while (!g_tntQuit && phase != Done && phase != Failed)
    {
        const double t = GetTickCount64() / 1000.0 - t0;
        if (t > timeout)
        {
            WLOG("tnt test: TIMEOUT in phase %s (row %d)", names[phase], row);
            fail("timeout");
            phase = Failed;
            break;
        }
        const control::Frame fr = control::OnFrame(0, 0.0f, 0.0f);
        const auto st = host::GetStatus();
        const bool inWorld = st.mcAlive && st.haveMcState && (st.mcFlags & proto::kMcInWorld);
        if (t - lastStep >= 0.033)
        {
            lastStep = t;
            if (fr.engaged && fr.eyeValid && fr.arrived)
            {
                geraltX = static_cast<float>(fr.feet[0]);
                geraltY = static_cast<float>(fr.feet[1]);
                geraltZ = static_cast<float>(fr.feet[2]);
            }
            float cx = geraltX, cy = geraltY, cz = geraltZ;
            host::SampleCentre(cx, cy, cz);
            std::string batch;
            const int bx = static_cast<int>(std::floor(cx)), bwy = static_cast<int>(std::floor(cy));
            for (int n = 0; n < 160; ++n)
            {
                const int idx = scanIndex++ % (41 * 41);
                const int wx = bx + idx % 41 - 20, wy = bwy + idx / 41 - 20;
                char item[48];
                std::snprintf(item, sizeof(item), "%d,%d,%.3f;", wx, wy, groundAt(wx, wy));
                batch += item;
            }
            host::PushHeights(batch);
            if (t - lastScan >= 0.5)
            {
                lastScan = t;
                scan(cx, cy, cz);
            }
            host::Tick(geraltX, geraltY, geraltZ, 180.0f - st.mcYaw, -st.mcPitch, host::kTickInGame);
        }

        combat::Event ev;
        while (combat::Pop(ev))
        {
            if (phase != Boom && phase != Check)
                continue;
            const Row& r = rows[row];
            if (ev.type == proto::kEvExplosion)
            {
                // W3 (x, y, z) = MC (x, -z, y)
                const double dx = ev.x - (r.x0 + 2.5), dz = -ev.y - (r.z + 0.5), dy = ev.z - r.y;
                const bool close = std::sqrt(dx * dx + dy * dy + dz * dz) < 8.0;
                ++totalBooms;
                if (close && std::fabs(ev.w - 4.0f) < 0.01f)
                {
                    ++booms;
                    lastBoomT = t;
                }
                WLOG("tnt test: %s: explosion %d at W3 (%.2f, %.2f, %.2f) radius %.1f%s", r.name, booms, ev.x, ev.y, ev.z, ev.w, close ? "" : " (not this row)");
            }
            else if (ev.type == proto::kEvHitActor && ev.id == banditId)
            {
                ++banditHits;
                WLOG("tnt test: %s: the bandit was hit: %.2f damage, knockback %.2f", r.name, ev.y, ev.z);
            }
        }

        switch (phase)
        {
        case WaitDriven:
            if (st.driven && inWorld && st.teleportAck == st.teleportSeq && !st.gateHolding && fr.eyeValid && t > 3.0)
            {
                WLOG("tnt test: Minecraft drives at W3 (%.2f, %.2f, %.2f)", st.mcX, -st.mcZ, st.mcY);
                cmd("gamemode creative");
                cmd("tag @s remove witchercraft_test_nodig");
                cmd("kill @e[type=minecraft:tnt]");
                phase = Setup;
                phaseT = t;
            }
            break;
        case Setup:
            if (t - phaseT > 2.0 && buildmode::CommandsPending() == 0)
            {
                phase = Place;
                phaseT = t;
            }
            break;
        case Place: {
            const Row& r = rows[row];
            WLOG("tnt test: ---- %s: 5 TNT at MC %d..%d %d %d", r.name, r.x0, r.x0 + 4, r.y, r.z);
            cmd(r.noDig ? "tag @s add witchercraft_test_nodig" : "tag @s remove witchercraft_test_nodig");
            cmd("fill %d %d %d %d %d %d minecraft:tnt", r.x0, r.y, r.z, r.x0 + 4, r.y, r.z);
            if (r.bandit)
            {
                char rec[160];
                // 3 m south of the middle of the row (MC +z = W3 -y), on the street.
                std::snprintf(rec, sizeof(rec), "%d,%.2f,%.2f,%.2f,0.0,0.60,1.80,1.000,1,10,Blast Bandit;", banditId, r.x0 + 2.5, -(r.z + 3.5), streetTop);
                combat::SetActors(rec);
            }
            else
                combat::SetActors("");
            booms = 0;
            banditHits = 0;
            phase = Ignite;
            phaseT = t;
            break;
        }
        case Ignite:
            if (t - phaseT > 2.0 && buildmode::CommandsPending() == 0)
            {
                const Row& r = rows[row];
                cmd("setblock %d %d %d minecraft:redstone_block", r.fromEast ? r.x0 + 5 : r.x0 - 1, r.y, r.z);
                phase = Boom;
                phaseT = t;
                lastBoomT = t;
            }
            break;
        case Boom:
            // Lit TNT: 4 s; each one it sets off: 0.5-1.5 s.
            if ((booms >= 5 && t - lastBoomT > 3.0) || t - lastBoomT > 8.0 || t - phaseT > 25.0)
            {
                const Row& r = rows[row];
                WLOG("tnt test: %s: %d of 5 TNT exploded", r.name, booms);
                log0 = McLogSize();
                cmd("fill %d %d %d %d %d %d minecraft:air replace minecraft:tnt", r.x0, r.y, r.z, r.x0 + 4, r.y, r.z);
                phase = Check;
                phaseT = t;
            }
            break;
        case Check: {
            const Row& r = rows[row];
            const bool none = McLogHas("No blocks were filled", log0), some = McLogHas("Successfully filled", log0);
            if (none || some || t - phaseT > 6.0)
            {
                const bool banditOk = !r.bandit || banditHits > 0;
                rowOk[row] = booms == 5 && none && banditOk;
                WLOG("tnt test: %s: %d explosion(s), TNT left: %s%s -> %s", r.name, booms, none ? "none" : some ? "YES" : "unknown (no /fill answer)",
                     r.bandit ? (banditHits > 0 ? ", bandit hit" : ", bandit NOT hit") : "", rowOk[row] ? "OK" : "BAD");
                if (!rowOk[row])
                    fail(std::string(r.name) + ": " + std::to_string(booms) + " of 5 exploded" + (none ? "" : ", TNT left") + (banditOk ? "" : ", bandit not hit"));
                cmd("setblock %d %d %d minecraft:air", r.fromEast ? r.x0 + 5 : r.x0 - 1, r.y, r.z);
                cmd("kill @e[type=minecraft:tnt]");
                combat::SetActors("");
                if (++row >= kRows)
                    phase = Done;
                else
                {
                    phase = Setup;
                    phaseT = t;
                }
            }
            break;
        }
        default:
            break;
        }
        if (t - lastPrint >= 2.0)
        {
            lastPrint = t;
            WLOG("t=%5.1fs %s row %d | %s", t, names[phase], row, host::StatusLine().c_str());
        }
        Sleep(8);
    }
    combat::SetActors("");
    buildmode::QueueCommand("tag @s remove witchercraft_test_nodig");
    buildmode::QueueCommand("kill @e[type=minecraft:tnt]");
    buildmode::QueueCommand("gamemode survival");
    // Let the clean-up commands finish typing (a chat screen left open would eat the next test's keys).
    for (int i = 0; i < 400 && (i < 90 || buildmode::CommandsPending() > 0); ++i)
    {
        control::OnFrame(0, 0.0f, 0.0f);
        host::Tick(geraltX, geraltY, geraltZ, 0.0f, 0.0f, host::kTickInGame);
        Sleep(33);
    }
    const bool pass = phase == Done && fails.empty() && rowOk[0] && rowOk[1] && rowOk[2];
    WLOG("tnt test: %s (street %d, ground %d, street without destruction %d; %d explosions in all) %s", pass ? "PASS" : "FAIL", rowOk[0], rowOk[1],
         rowOk[2], totalBooms, fails.c_str());
    host::Stop();
    return pass ? 0 : 2;
}
