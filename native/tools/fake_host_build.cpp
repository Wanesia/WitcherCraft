// ---- WitcherCraft building offline test: placing blocks on sloped Witcher ground, TNT ------------
//
//   fake_host.exe build [timeoutSeconds=240]
//
// Build mode (Minecraft looks where the host aims, its own crosshair pick places the blocks) on
// Witcher ground that slopes 0.25 m per metre along W3 x, in a fresh spot every run:
//   A. stone on the ground 2.5 m ahead: the block lands in the cell under the crosshair;
//   B. stone on the ground right beside A (one cell along the slope, where the ground is 0.25 m
//      higher): it must land in A's row (SkyClip.snapToNeighbourRow), not a row above it;
//   C. TNT on the ground two cells west of A, then flint and steel on the TNT's top: the TNT must
//      be lit (its block goes away, primed).
// Blocks are read back through the delta API the script polls (W3 cells). Exit code 0 = PASS.
#include "blocks.hpp"
#include "buildmode.hpp"
#include "control.hpp"
#include "log.hpp"
#include "skycraft_host.hpp"
#include "skycraft_protocol.h"

#include <Windows.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
volatile bool g_buildQuit = false;
BOOL WINAPI OnCtrlBuild(DWORD)
{
    g_buildQuit = true;
    return TRUE;
}

struct BuildDelta
{
    bool add;
    int id, x, y, z;
};

std::vector<BuildDelta> ParseBuildDeltas(const std::string& s)
{
    std::vector<BuildDelta> out;
    size_t i = 0;
    while (i < s.size())
    {
        size_t end = s.find(';', i);
        if (end == std::string::npos)
            end = s.size();
        const std::string item = s.substr(i, end - i);
        i = end + 1;
        BuildDelta d{};
        int cls = 0;
        if (item.size() > 1 && item[0] == '+' && std::sscanf(item.c_str() + 1, "%d,%d,%d,%d,%d", &d.id, &d.x, &d.y, &d.z, &cls) == 5)
        {
            d.add = true;
            out.push_back(d);
        }
        else if (item.size() > 1 && item[0] == '-' && std::sscanf(item.c_str() + 1, "%d", &d.id) == 1)
            out.push_back(d);
    }
    return out;
}
} // namespace

int RunBuildTest(int timeout)
{
    using namespace wc;
    namespace proto = skycraft::proto;
    SetConsoleCtrlHandler(OnCtrlBuild, TRUE);
    const uint64_t seed = (GetTickCount64() / 1000) % 200;
    const int x0 = 900 + static_cast<int>(seed) * 150, y0 = -300;
    const float base = 64.325f, slope = 0.25f;
    auto groundAt = [&](float x) { return base + slope * (x - static_cast<float>(x0)); };
    const float px = x0 + 0.5f, py = y0 + 0.5f, pz = groundAt(px);
    WLOG("build test: player at W3 (%.2f, %.2f, %.2f), ground %.3f + %.2f per m along x, timeout %d s", px, py, pz, base, slope, timeout);
    control::SetMcMode(false); // build mode (F7) only exists in Witcher mode
    if (!host::Start())
        return 1;
    host::SetWorld("fake\\build_test.w2w");

    // Third-person camera 2 m behind and 2.2 m above the feet, as in the blocks test.
    const float cam[3] = {px, py - 2.0f, pz + 2.2f};
    struct Step
    {
        const char* name;
        const char* item;   // put in the main hand first (nullptr: keep)
        float target[3];    // the point Minecraft looks at
        int expX, expY, expZ; // the cell the new block must be in (or the TNT to light: -1 = remove)
        bool light;         // flint and steel on the TNT placed in the previous step
    };
    const float ax = x0 + 0.5f, bx = x0 + 1.5f, tx = x0 - 1.5f; // C/D: west of A, within reach (5 blocks)
    // Expected rows: A's ground 64.45 -> floor(64.45 + 0.4) = 64; B's ground 64.70 alone would give 65;
    // the TNT's ground 63.95 -> 64 (no neighbour: x0 - 1 stays empty).
    Step steps[] = {
        {"A: stone on the ground", "minecraft:stone 64", {ax, y0 + 2.5f, groundAt(ax)}, x0, y0 + 2, 64, false},
        {"B: stone on the ground beside A (same row)", nullptr, {bx, y0 + 2.5f, groundAt(bx)}, x0 + 1, y0 + 2, 64, false},
        {"C: TNT on the ground", "minecraft:tnt 64", {tx, y0 + 2.5f, groundAt(tx)}, x0 - 2, y0 + 2, 64, false},
        {"D: flint and steel on the TNT", "minecraft:flint_and_steel", {tx, y0 + 2.5f, 65.0f}, 0, 0, 0, true},
    };
    const int stepCount = static_cast<int>(sizeof(steps) / sizeof(steps[0]));

    enum Phase
    {
        WaitLink,
        Kit,
        Item,
        Click,
        WaitResult,
        Done,
        Failed
    };
    Phase phase = WaitLink;
    int step = 0, tries = 0, tntId = -1;
    std::string fails, allDeltas;
    const double t0 = GetTickCount64() / 1000.0;
    double phaseT = 0, lastPrint = -1;
    int scanIndex = 0;
    while (!g_buildQuit && phase != Done && phase != Failed)
    {
        const double t = GetTickCount64() / 1000.0 - t0;
        if (t > timeout)
        {
            WLOG("build test: TIMEOUT in phase %d step %d", phase, step);
            fails += " timeout";
            phase = Failed;
            break;
        }
        std::string batch;
        for (int n = 0; n < 160; ++n)
        {
            const int idx = scanIndex++ % (33 * 33);
            const int sx = x0 + idx % 33 - 16, sy = y0 + idx / 33 - 16;
            char item[64];
            std::snprintf(item, sizeof(item), "%d,%d,%.3f;", sx, sy, groundAt(static_cast<float>(sx)));
            batch += item;
        }
        host::PushHeights(batch);
        host::Tick(px, py, pz, 0.0f, -30.0f, host::kTickInGame);
        const auto st = host::GetStatus();

        const std::string polled = blocks::Poll(64);
        if (!polled.empty())
        {
            WLOG("build test: poll -> %s", polled.c_str());
            allDeltas += polled;
        }
        for (const auto& d : ParseBuildDeltas(polled))
        {
            if (phase != WaitResult)
                continue;
            const Step& s = steps[step];
            if (!s.light && d.add)
            {
                const bool ok = d.x == s.expX && d.y == s.expY && d.z == s.expZ;
                WLOG("build test: %s -> block id %d at W3 (%d, %d, %d), expected (%d, %d, %d): %s", s.name, d.id, d.x, d.y, d.z, s.expX, s.expY,
                     s.expZ, ok ? "OK" : "WRONG CELL");
                if (!ok)
                {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf), " [%s: got (%d,%d,%d) want (%d,%d,%d)]", s.name, d.x, d.y, d.z, s.expX, s.expY, s.expZ);
                    fails += buf;
                }
                if (s.item && std::string(s.item).rfind("minecraft:tnt", 0) == 0)
                    tntId = d.id;
                ++step, tries = 0, phase = step < stepCount ? Item : Done, phaseT = t;
            }
            else if (s.light && !d.add && d.id == tntId)
            {
                WLOG("build test: %s -> the TNT block (id %d) went away: lit, OK", s.name, d.id);
                ++step, tries = 0, phase = step < stepCount ? Item : Done, phaseT = t;
            }
        }

        switch (phase)
        {
        case WaitLink:
            if (st.mcAlive && st.haveMcState && (st.mcFlags & 1u) && st.teleportAck == st.teleportSeq &&
                std::hypot(st.mcX - px, -st.mcZ - py) < 0.5 && std::fabs(st.mcY - pz) < 0.5)
            {
                WLOG("build test: Minecraft player arrived (t=%.1f s); build mode ON", t);
                buildmode::SetEnabled(true);
                phase = Kit;
                phaseT = t;
            }
            break;
        case Kit:
            if (t - phaseT > 2.0 && buildmode::CommandsPending() == 0)
            {
                phase = Item;
                phaseT = t;
            }
            break;
        case Item:
            if (steps[step].item)
                buildmode::QueueCommand(std::string("item replace entity @s weapon.mainhand with ") + steps[step].item);
            phase = Click;
            phaseT = t;
            break;
        case Click:
        case WaitResult: {
            const Step& s = steps[step];
            const float dir[3] = {s.target[0] - cam[0], s.target[1] - cam[1], s.target[2] - cam[2]};
            const auto aim = buildmode::SetAim(true, cam, dir, s.target, true);
            const double since = t - phaseT;
            if (phase == Click && since > 1.2 && buildmode::CommandsPending() == 0)
            {
                WLOG("build test: %s: aim (%.2f, %.2f, %.2f) w=%.0f, MC yaw %.1f pitch %.1f; RIGHT CLICK", s.name, aim.x, aim.y, aim.z, aim.w, st.mcYaw,
                     st.mcPitch);
                buildmode::QueueInput(proto::kInMouseButton, 3, 1);
                Sleep(100);
                buildmode::QueueInput(proto::kInMouseButton, 3, 0);
                phase = WaitResult;
                phaseT = t;
                ++tries;
            }
            else if (phase == WaitResult && since > 4.0)
            {
                if (tries >= 3)
                {
                    WLOG("build test: FAIL: %s: nothing happened after 3 clicks", s.name);
                    fails += std::string(" [") + s.name + ": nothing happened]";
                    ++step, tries = 0, phase = step < stepCount ? Item : Done, phaseT = t;
                }
                else
                {
                    phase = Click;
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
            WLOG("t=%5.1fs phase %d step %d | %s", t, phase, step, host::StatusLine().c_str());
        }
        Sleep(33);
    }
    buildmode::SetAim(false, nullptr, nullptr, nullptr, false);
    buildmode::SetEnabled(false);
    for (int i = 0; i < 30; ++i)
    {
        host::Tick(px, py, pz, 0.0f, -30.0f, host::kTickInGame);
        Sleep(33);
    }
    const bool pass = phase == Done && fails.empty();
    WLOG("build test: all deltas: %s", allDeltas.c_str());
    WLOG("build test: %s%s; %s", pass ? "PASS" : "FAIL", fails.c_str(), host::StatusLine().c_str());
    host::Stop();
    return pass ? 0 : 2;
}
