// Offline link test: stands in for The Witcher 3. Uses the SAME host code as dinput8.dll
// (src/skycraft_host.cpp), so it creates `Local\SkyCraft_v1` exactly like the game does.
//
//   fake_host.exe [seconds=120] [groundZ=64] [speed=4]
//
// Simulates what witchercraft.ws does every ~33 ms: a 33x33 ground height field around the
// player (flat at groundZ with a gentle 1:10 ramp on +x so the triangles aren't trivial),
// pushed through the batched string API, and HostTick with a player walking a 24 m circle.
// Prints the McState (MC player in W3 coords) once a second.
#include "blocks.hpp"
#include "buildmode.hpp"
#include "combat.hpp"
#include "control.hpp"
#include "log.hpp"
#include "skycraft_host.hpp"
#include "skycraft_protocol.h"

#include <Windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
float Ground(int wx, int /*wy*/, float base)
{
    // Flat, plus a 1:10 slope for x > 20 (walkable, exercises the triangle collider).
    return wx > 20 ? base + (wx - 20) * 0.1f : base;
}
volatile bool g_quit = false;
BOOL WINAPI OnCtrl(DWORD)
{
    g_quit = true;
    return TRUE;
}
} // namespace

// ---- M2b offline test: place and break a block through the real Minecraft ---------------------
//
//   fake_host.exe blocks [timeoutSeconds=240]
//
// A player stands still on flat ground (W3 z = 64) in a fresh spot (changes every run, so old
// blocks in the persistent MC world don't interfere). Build mode on -> the host types the kit
// commands (creative + hotbar). Then: aim at the ground 2.5 m ahead, right-click, expect the
// delta API to report "+id,x,y,z,c" for the expected W3 block; aim at that block (found by the
// native block raycast), left-click, expect "-id;". Exit code 0 = PASS.
namespace
{
struct Delta
{
    bool add;
    int id, x, y, z, cls;
};

std::vector<Delta> ParseDeltas(const std::string& s)
{
    std::vector<Delta> out;
    size_t i = 0;
    while (i < s.size())
    {
        size_t end = s.find(';', i);
        if (end == std::string::npos)
            end = s.size();
        const std::string item = s.substr(i, end - i);
        i = end + 1;
        Delta d{};
        if (item.size() > 1 && item[0] == '+' && std::sscanf(item.c_str() + 1, "%d,%d,%d,%d,%d", &d.id, &d.x, &d.y, &d.z, &d.cls) == 5)
        {
            d.add = true;
            out.push_back(d);
        }
        else if (item.size() > 1 && item[0] == '-' && std::sscanf(item.c_str() + 1, "%d", &d.id) == 1)
        {
            out.push_back(d);
        }
    }
    return out;
}

int RunBlocksTest(int timeout)
{
    using namespace wc;
    const float ground = 64.0f;
    const uint64_t seed = (GetTickCount64() / 1000) % 200;
    const float px = 500.0f + static_cast<float>(seed) * 150.0f + 0.5f, py = -200.3f, pz = ground;
    WLOG("blocks test: player at W3 (%.2f, %.2f, %.2f), timeout %d s", px, py, pz, timeout);
    control::SetMcMode(false); // build mode (F7) only exists in Witcher mode since M3a
    if (!host::Start())
        return 1;
    host::SetWorld("fake\\blocks_test.w2w");

    enum Phase
    {
        WaitLink,
        Kit,
        Place,
        WaitAdd,
        Break,
        WaitRemove,
        Done,
        Failed
    };
    Phase phase = WaitLink;
    const double t0 = GetTickCount64() / 1000.0;
    double phaseT = 0, lastPrint = -1;
    int scanIndex = 0;
    int clickTries = 0;
    int placedId = -1, placedX = 0, placedY = 0, placedZ = 0, placedCls = -1;
    // Aim: from a third-person camera 2 m behind and 2.2 m above the feet (W3 heading 0 = +Y).
    const float cam[3] = {px, py - 2.0f, pz + 2.2f};
    const float groundTarget[3] = {px, py + 2.5f, ground};
    const int expX = static_cast<int>(std::floor(groundTarget[0])), expY = static_cast<int>(std::floor(groundTarget[1])), expZ = 64;
    std::string allDeltas;
    while (!g_quit && phase != Done && phase != Failed)
    {
        const double t = GetTickCount64() / 1000.0 - t0;
        if (t > timeout)
        {
            WLOG("blocks test: TIMEOUT in phase %d", phase);
            phase = Failed;
            break;
        }
        std::string batch;
        const int bx = static_cast<int>(std::floor(px)), by = static_cast<int>(std::floor(py));
        for (int n = 0; n < 160; ++n)
        {
            const int idx = scanIndex++ % (33 * 33);
            char item[64];
            std::snprintf(item, sizeof(item), "%d,%d,%.3f;", bx + idx % 33 - 16, by + idx / 33 - 16, ground);
            batch += item;
        }
        host::PushHeights(batch);
        host::Tick(px, py, pz, 0.0f, -30.0f, host::kTickInGame);
        const auto st = host::GetStatus();

        // Deltas every tick (the script polls like this).
        const std::string polled = blocks::Poll(64);
        if (!polled.empty())
        {
            WLOG("blocks test: poll -> %s", polled.c_str());
            allDeltas += polled;
        }
        for (const auto& d : ParseDeltas(polled))
        {
            if (d.add && phase == WaitAdd && d.x == expX && d.y == expY && d.z == expZ)
            {
                placedId = d.id, placedX = d.x, placedY = d.y, placedZ = d.z, placedCls = d.cls;
                WLOG("blocks test: PLACED block id %d at W3 (%d, %d, %d) class %d (expected (%d, %d, %d))", d.id, d.x, d.y, d.z, d.cls, expX, expY, expZ);
                phase = Break;
                phaseT = t;
                clickTries = 0;
            }
            else if (!d.add && phase == WaitRemove && d.id == placedId)
            {
                WLOG("blocks test: REMOVED block id %d", d.id);
                phase = Done;
            }
        }

        switch (phase)
        {
        case WaitLink:
            if (st.mcAlive && st.haveMcState && (st.mcFlags & 1u) && st.teleportAck == st.teleportSeq &&
                std::hypot(st.mcX - px, -st.mcZ - py) < 0.5 && std::fabs(st.mcY - pz) < 0.5)
            {
                WLOG("blocks test: Minecraft player arrived (t=%.1f s); build mode ON", t);
                buildmode::SetEnabled(true);
                phase = Kit;
                phaseT = t;
            }
            break;
        case Kit:
            if (t - phaseT > 2.0 && buildmode::CommandsPending() == 0)
            {
                WLOG("blocks test: kit commands done (t=%.1f s)", t);
                phase = Place;
                phaseT = t;
            }
            break;
        case Place:
        case WaitAdd: {
            const float dir[3] = {groundTarget[0] - cam[0], groundTarget[1] - cam[1], groundTarget[2] - cam[2]};
            const auto aim = buildmode::SetAim(true, cam, dir, groundTarget, true);
            const double since = t - phaseT;
            if (phase == Place && since > 0.8)
            {
                WLOG("blocks test: aim (%.2f, %.2f, %.2f) w=%.0f, MC yaw %.1f pitch %.1f; RIGHT CLICK", aim.x, aim.y, aim.z, aim.w, st.mcYaw, st.mcPitch);
                buildmode::QueueInput(skycraft::proto::kInMouseButton, 3, 1);
                Sleep(100);
                buildmode::QueueInput(skycraft::proto::kInMouseButton, 3, 0);
                phase = WaitAdd;
                phaseT = t;
                ++clickTries;
            }
            else if (phase == WaitAdd && since > 5.0)
            {
                if (clickTries >= 3)
                {
                    WLOG("blocks test: FAIL: no block appeared at W3 (%d, %d, %d)", expX, expY, expZ);
                    phase = Failed;
                }
                else
                {
                    phase = Place;
                    phaseT = t;
                }
            }
            break;
        }
        case Break:
        case WaitRemove: {
            const float top[3] = {placedX + 0.5f, placedY + 0.5f, placedZ + 1.0f};
            const float dir[3] = {top[0] - cam[0], top[1] - cam[1], top[2] - cam[2]};
            const float none[3] = {0, 0, 0};
            const auto aim = buildmode::SetAim(true, cam, dir, none, false); // only the native block raycast
            const double since = t - phaseT;
            if (phase == Break && since > 0.8)
            {
                WLOG("blocks test: block raycast aim (%.2f, %.2f, %.2f) w=%.0f, MC yaw %.1f pitch %.1f; LEFT CLICK", aim.x, aim.y, aim.z, aim.w, st.mcYaw,
                     st.mcPitch);
                buildmode::QueueInput(skycraft::proto::kInMouseButton, 1, 1);
                Sleep(100);
                buildmode::QueueInput(skycraft::proto::kInMouseButton, 1, 0);
                phase = WaitRemove;
                phaseT = t;
                ++clickTries;
            }
            else if (phase == WaitRemove && since > 5.0)
            {
                if (clickTries >= 3)
                {
                    WLOG("blocks test: FAIL: block id %d was not removed", placedId);
                    phase = Failed;
                }
                else
                {
                    phase = Break;
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
            WLOG("t=%5.1fs phase %d | %s", t, phase, host::StatusLine().c_str());
        }
        Sleep(33);
    }
    buildmode::SetAim(false, nullptr, nullptr, nullptr, false);
    buildmode::SetEnabled(false);
    for (int i = 0; i < 30; ++i) // let the release reach MC
    {
        host::Tick(px, py, pz, 0.0f, -30.0f, host::kTickInGame);
        Sleep(33);
    }
    WLOG("blocks test: all deltas: %s", allDeltas.c_str());
    WLOG("blocks test: %s (placed id %d class %d at W3 (%d, %d, %d)); %s", phase == Done ? "PASS" : "FAIL", placedId, placedCls, placedX, placedY,
         placedZ, host::StatusLine().c_str());
    host::Stop();
    return phase == Done ? 0 : 2;
}
} // namespace

// ---- M3a offline test: Minecraft drives the player ---------------------------------------------
//
//   fake_host.exe control [timeoutSeconds=300]
//
// Simulates the script exactly like Minecraft mode in game: WCraft_ControlFrame every ~16 ms (a
// 60 fps frame), and every ~33 ms a step that teleports "Geralt" to Minecraft's interpolated feet,
// ticks the host and samples the ground around host::SampleCentre(). Ground: flat at W3 z = 64,
// except NO ground (void, like missing collision) for W3 y > startY + 18. Input is forwarded the
// same way the input hook does it (buildmode::QueueEvent / control::InjectMouse).
// Checks: hold gate on arrival, W walks forward, Space jumps and lands, the mouse turns the look
// (and walking follows it), the safety net rescues a fall into the void, and the per-frame eye
// interpolation is smooth (no 20 Hz stair steps). Exit code 0 = PASS.
namespace
{
int RunControlTest(int timeout)
{
    using namespace wc;
    namespace proto = skycraft::proto;
    const float ground = 64.0f;
    const uint64_t seed = (GetTickCount64() / 1000) % 200;
    const float startX = -3000.0f - static_cast<float>(seed) * 150.0f + 0.5f, startY = 400.5f;
    const float voidY = startY + 18.0f;
    auto groundAt = [&](int /*wx*/, int wy) { return static_cast<float>(wy) > voidY ? NAN : ground; };
    WLOG("control test: start W3 (%.1f, %.1f, %.1f), void for y > %.1f, timeout %d s", startX, startY, ground, voidY, timeout);
    control::SetMcMode(true);
    if (!host::Start())
        return 1;
    host::SetWorld("fake\\control_test.w2w");

    enum Phase
    {
        WaitDriven,
        Survival,
        Walk,
        Jump,
        Turn,
        TurnWalk,
        Void,
        Settle,
        Done,
        Failed
    };
    const char* names[] = {"WaitDriven", "Survival", "Walk", "Jump", "Turn", "TurnWalk", "Void", "Settle", "Done", "Failed"};
    Phase phase = WaitDriven;
    double phaseT = 0;
    std::string fails;
    auto fail = [&](const char* what) {
        WLOG("control test: FAIL: %s", what);
        fails += what;
        fails += "; ";
    };

    float geraltX = startX, geraltY = startY, geraltZ = ground;
    int scanIndex = 0;
    const double t0 = GetTickCount64() / 1000.0;
    double lastStep = -1, lastPrint = -1;
    double p0x = 0, p0y = 0, p0z = 0; // MC feet (W3 coords) at phase start
    double maxZ = 0;
    double minZBeforeVoid = 1e9;
    bool sawGateHold = false;
    uint64_t rescues0 = 0;
    float yaw0 = 0;
    // Smoothness while walking: per-frame eye steps
    double prevEye[3] = {};
    bool havePrevEye = false;
    int walkFrames = 0, stillFrames = 0;
    double stepSum = 0, stepMax = 0;
    bool walkOk = false, jumpOk = false, turnOk = false, voidOk = false, smoothOk = false;

    while (!g_quit && phase != Done && phase != Failed)
    {
        const double t = GetTickCount64() / 1000.0 - t0;
        if (t > timeout)
        {
            WLOG("control test: TIMEOUT in phase %s", names[phase]);
            fail("timeout");
            phase = Failed;
            break;
        }

        // ---- "every frame" (script's per-frame loop) ----
        const control::Frame fr = control::OnFrame(0, 0.0f, 0.0f);
        const auto st = host::GetStatus();
        if (st.gateHolding)
            sawGateHold = true;
        const bool inWorld = st.mcAlive && st.haveMcState && (st.mcFlags & proto::kMcInWorld);
        const double mcW3x = st.mcX, mcW3y = -st.mcZ, mcW3z = st.mcY;

        if (fr.eyeValid && phase == Walk)
        {
            if (havePrevEye)
            {
                const double d = std::sqrt((fr.eye[0] - prevEye[0]) * (fr.eye[0] - prevEye[0]) + (fr.eye[1] - prevEye[1]) * (fr.eye[1] - prevEye[1]));
                if (t - phaseT > 0.6 && t - phaseT < 2.0) // steady walking
                {
                    ++walkFrames;
                    stepSum += d;
                    stepMax = std::max(stepMax, d);
                    if (d < 0.001)
                        ++stillFrames;
                }
            }
            std::memcpy(prevEye, fr.eye, sizeof(prevEye));
            havePrevEye = true;
        }

        // ---- "every step" (~33 ms): Geralt follows MC's feet, host tick, ground samples ----
        if (t - lastStep >= 0.033)
        {
            lastStep = t;
            if (fr.engaged && fr.eyeValid && fr.arrived) // as the script: WCraft_ControlFeet().W == 1
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
                const float h = groundAt(wx, wy);
                if (std::isnan(h))
                    std::snprintf(item, sizeof(item), "%d,%d,n;", wx, wy);
                else
                    std::snprintf(item, sizeof(item), "%d,%d,%.3f;", wx, wy, h);
                batch += item;
            }
            host::PushHeights(batch);
            host::Tick(geraltX, geraltY, geraltZ, 180.0f - st.mcYaw, -st.mcPitch, host::kTickInGame);
        }

        if (inWorld && phase >= Survival && phase < Void)
            minZBeforeVoid = std::min(minZBeforeVoid, mcW3z);

        switch (phase)
        {
        case WaitDriven:
            if (st.driven && inWorld && st.teleportAck == st.teleportSeq && !st.gateHolding && fr.eyeValid)
            {
                WLOG("control test: Minecraft drives (t=%.1f s), MC at W3 (%.2f, %.2f, %.2f); hold gate seen: %s", t, mcW3x, mcW3y, mcW3z,
                     sawGateHold ? "yes" : "no");
                buildmode::QueueCommand("gamemode survival");
                // Face north: the walk/turn/void checks assume it, and the world keeps the last player's yaw.
                buildmode::QueueCommand("rotate @s 180 0");
                phase = Survival;
                phaseT = t;
            }
            break;
        case Survival:
            if (t - phaseT > 1.5 && buildmode::CommandsPending() == 0 && !(st.mcFlags & proto::kMcScreenOpen) && t - phaseT > 3.0)
            {
                p0x = mcW3x, p0y = mcW3y, p0z = mcW3z;
                WLOG("control test: WALK: W down for 2 s from (%.2f, %.2f, %.2f), look yaw %.1f", p0x, p0y, p0z, st.mcYaw);
                buildmode::QueueEvent({proto::kInKey, 26, 1, 0, 0});
                phase = Walk;
                phaseT = t;
            }
            break;
        case Walk:
            if (t - phaseT > 2.0 && t - phaseT < 2.05)
                buildmode::QueueEvent({proto::kInKey, 26, 0, 0, 0});
            if (t - phaseT > 2.8)
            {
                const double dy = mcW3y - p0y, dx = mcW3x - p0x, dz = mcW3z - p0z;
                walkOk = dy > 4.0 && std::fabs(dx) < 1.0 && std::fabs(dz) < 0.2;
                const double mean = walkFrames ? stepSum / walkFrames : 0;
                smoothOk = walkFrames > 30 && stillFrames * 10 < walkFrames && stepMax < mean * 3.0;
                WLOG("control test: walked (%.2f, %.2f, %.2f) m -> %s; eye per frame: %d frames, %d still (<1 mm), mean %.3f max %.3f m -> %s", dx, dy,
                     dz, walkOk ? "OK" : "BAD", walkFrames, stillFrames, mean, stepMax, smoothOk ? "smooth" : "JUDDER");
                if (!walkOk)
                    fail("walk");
                if (!smoothOk)
                    fail("smoothness");
                p0x = mcW3x, p0y = mcW3y, p0z = mcW3z;
                maxZ = mcW3z;
                buildmode::QueueEvent({proto::kInKey, 44, 1, 0, 0});
                WLOG("control test: JUMP: Space at z %.2f", p0z);
                phase = Jump;
                phaseT = t;
            }
            break;
        case Jump:
            maxZ = std::max(maxZ, mcW3z);
            if (t - phaseT > 0.15 && t - phaseT < 0.2)
                buildmode::QueueEvent({proto::kInKey, 44, 0, 0, 0});
            if (t - phaseT > 2.5)
            {
                const bool landed = (st.mcFlags & proto::kMcOnGround) && std::fabs(mcW3z - p0z) < 0.05;
                jumpOk = maxZ - p0z > 1.0 && landed;
                WLOG("control test: jump apex +%.2f m, landed %s (z %.2f) -> %s", maxZ - p0z, landed ? "yes" : "no", mcW3z, jumpOk ? "OK" : "BAD");
                if (!jumpOk)
                    fail("jump");
                yaw0 = st.mcYaw;
                control::InjectMouse(600, 0); // 600 counts * 0.15 deg (sensitivity 0.5) = 90 deg right
                phase = Turn;
                phaseT = t;
            }
            break;
        case Turn:
            if (t - phaseT > 0.8)
            {
                float d = std::fmod(st.mcYaw - yaw0 + 540.0f, 360.0f) - 180.0f;
                turnOk = std::fabs(d - 90.0f) < 3.0f;
                WLOG("control test: TURN: MC yaw %.1f -> %.1f (%+.1f, expect +90) -> %s", yaw0, st.mcYaw, d, turnOk ? "OK" : "BAD");
                p0x = mcW3x, p0y = mcW3y, p0z = mcW3z;
                buildmode::QueueEvent({proto::kInKey, 26, 1, 0, 0});
                phase = TurnWalk;
                phaseT = t;
            }
            break;
        case TurnWalk:
            if (t - phaseT > 1.0)
            {
                buildmode::QueueEvent({proto::kInKey, 26, 0, 0, 0});
                // Yaw +90 from north (MC 180 -> 270 = -90) faces east: W3 +x.
                const double dx = mcW3x - p0x, dy = mcW3y - p0y;
                const bool dirOk = dx > 2.5 && std::fabs(dy) < 1.0;
                WLOG("control test: walked (%.2f, %.2f) after the turn -> %s", dx, dy, dirOk ? "OK (east)" : "BAD");
                turnOk = turnOk && dirOk;
                if (!turnOk)
                    fail("turn");
                control::InjectMouse(-600, 0);
                rescues0 = st.rescues;
                phase = Void;
                phaseT = t;
            }
            break;
        case Void:
            if (t - phaseT > 0.5 && t - phaseT < 0.55)
            {
                WLOG("control test: VOID: walking north into the area without ground (y > %.1f) from y %.2f", voidY, mcW3y);
                buildmode::QueueEvent({proto::kInKey, 26, 1, 0, 0});
            }
            if (st.rescues > rescues0)
            {
                buildmode::QueueEvent({proto::kInKey, 26, 0, 0, 0});
                WLOG("control test: safety net fired (t=%.1f s)", t);
                phase = Settle;
                phaseT = t;
            }
            else if (t - phaseT > 15.0)
            {
                buildmode::QueueEvent({proto::kInKey, 26, 0, 0, 0});
                fail("void: no rescue");
                phase = Failed;
            }
            break;
        case Settle:
            if (t - phaseT > 4.0)
            {
                voidOk = inWorld && std::fabs(mcW3z - ground) < 0.2 && mcW3y < voidY && (st.mcFlags & proto::kMcOnGround);
                WLOG("control test: after the rescue MC at (%.2f, %.2f, %.2f) on ground %s -> %s", mcW3x, mcW3y, mcW3z,
                     (st.mcFlags & proto::kMcOnGround) ? "yes" : "no", voidOk ? "OK" : "BAD");
                if (!voidOk)
                    fail("rescue");
                phase = Done;
            }
            break;
        default:
            break;
        }
        if (t - lastPrint >= 2.0)
        {
            lastPrint = t;
            WLOG("t=%5.1fs %s | Geralt (%.2f, %.2f, %.2f) | %s", t, names[phase], geraltX, geraltY, geraltZ, host::StatusLine().c_str());
        }
        Sleep(16);
    }
    for (int i = 0; i < 30; ++i) // let the key releases reach MC
    {
        control::OnFrame(0, 0.0f, 0.0f);
        host::Tick(geraltX, geraltY, geraltZ, 0.0f, 0.0f, host::kTickInGame);
        Sleep(33);
    }
    const auto m = control::Motion();
    const bool noFall = minZBeforeVoid > ground - 0.5;
    if (!noFall)
        fail("fell below the ground before the void phase");
    const bool pass = phase == Done && fails.empty() && walkOk && jumpOk && turnOk && voidOk && smoothOk && noFall;
    WLOG("control test: hold gate seen %s; lowest MC z before the void %.2f; interpolation: %llu frames, %llu late, %llu ticks, render delay %.1f ms",
         sawGateHold ? "yes" : "no", minZBeforeVoid, static_cast<unsigned long long>(m.frames), static_cast<unsigned long long>(m.lateFrames),
         static_cast<unsigned long long>(m.ticks), m.renderDelayMs);
    WLOG("control test: %s (walk %d jump %d turn %d smooth %d rescue %d) %s; %s", pass ? "PASS" : "FAIL", walkOk, jumpOk, turnOk, smoothOk, voidOk,
         fails.c_str(), host::StatusLine().c_str());
    host::Stop();
    return pass ? 0 : 2;
}
} // namespace

// ---- M4a offline test: combat through the real Minecraft ------------------------------------
//
//   fake_host.exe combat [timeoutSeconds=300]
//
// Minecraft mode on flat ground (as the control test). Survival, Normal, a diamond sword in hand.
// One fake W3 actor ("Bandit", a negative id to check the signed round trip) is published 2.5 m in
// front of Steve through the same batched string the script uses; LMB must produce a kEvHitActor
// with that id and ~7 damage. A charged crossbow fired at it (right click) must come back as a
// kEvHitActor flagged kHitProjectile, weapon kWeaponArrow (the Witcher knocks flying monsters down
// on those, as with Geralt's bolts). Then kInHurt: a 10 % hit must lower Steve's health in
// Minecraft's log; a lethal one must come back as kEvPlayerDied. Batched magic damage and the
// respawn are reported too. Exit code 0 = PASS.
namespace
{
std::string McLogPath()
{
    if (const char* env = std::getenv("WC_MC_LOG")) // another Minecraft (e.g. the release's Prism instance)
        return env;
    char exe[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string p = exe; // ...\native\build-combat\Release\fake_host.exe
    for (int i = 0; i < 4; ++i)
        p = p.substr(0, p.find_last_of('\\'));
    return p + "\\minecraft\\run\\logs\\latest.log";
}

// Minecraft's log from byte `from` on (shared read: Minecraft keeps writing it).
std::string ReadFrom(const std::string& path, uint64_t from, uint64_t* sizeOut)
{
    std::string out;
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return out;
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    if (sizeOut)
        *sizeOut = static_cast<uint64_t>(size.QuadPart);
    if (static_cast<uint64_t>(size.QuadPart) > from)
    {
        LARGE_INTEGER pos;
        pos.QuadPart = static_cast<LONGLONG>(from);
        SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
        out.resize(static_cast<size_t>(size.QuadPart - from));
        DWORD got = 0;
        ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
        out.resize(got);
    }
    CloseHandle(f);
    return out;
}

// "The Witcher hit the player for 15.0 (3.0 Minecraft): health 20.0 -> 17.0" (SkyCombat.hurtPlayer)
bool FindHurtLine(const std::string& text, float& mc, float& before, float& after)
{
    const char* key = "The Witcher hit the player for ";
    const size_t at = text.find(key);
    if (at == std::string::npos)
        return false;
    float sky = 0;
    return std::sscanf(text.c_str() + at + std::strlen(key), "%f (%f Minecraft): health %f -> %f", &sky, &mc, &before, &after) == 4;
}

int RunCombatTest(int timeout)
{
    using namespace wc;
    namespace proto = skycraft::proto;
    const float ground = 64.0f;
    const uint64_t seed = (GetTickCount64() / 1000) % 200;
    const float startX = 3000.0f + static_cast<float>(seed) * 150.0f + 0.5f, startY = -400.5f;
    const int32_t actorId = -1234567; // GetGuidHash() is a signed int
    const std::string logPath = McLogPath();
    WLOG("combat test: start W3 (%.1f, %.1f, %.1f), timeout %d s, MC log %s", startX, startY, ground, timeout, logPath.c_str());
    control::SetMcMode(true);
    if (!host::Start())
        return 1;
    host::SetWorld("fake\\combat_test.w2w");

    enum Phase
    {
        WaitDriven,
        Setup,
        Spawn,
        Attack,
        Arrow,
        Hurt,
        Reach,
        ReachBack,
        Lethal,
        Respawn,
        Dot,
        Done,
        Failed
    };
    const char* names[] = {"WaitDriven", "Setup", "Spawn", "Attack", "Arrow", "Hurt", "Reach", "ReachBack", "Lethal", "Respawn", "Dot", "Done", "Failed"};
    Phase phase = WaitDriven;
    double phaseT = 0;
    std::string fails;
    auto fail = [&](const char* what) {
        WLOG("combat test: FAIL: %s", what);
        fails += what;
        fails += "; ";
    };

    float geraltX = startX, geraltY = startY, geraltZ = ground;
    float actorX = 0, actorY = 0, actorZ = 0, actorHeading = 0;
    bool havePublishedActor = false;
    int scanIndex = 0, clicks = 0;
    const double t0 = GetTickCount64() / 1000.0;
    double lastStep = -1, lastPrint = -1, lastActors = -1;
    uint64_t logFrom = 0;
    bool hurtSent = false;
    bool hitOk = false, hurtOk = false, diedOk = false, respawnOk = false, dotOk = false;
    // Reach: the bandit 5 m below Steve (as when Steve stands on a pillar over the hidden Geralt):
    // its melee hit must be ignored ("out of reach"), Steve's health unchanged.
    bool reachOk = false, reachSent = false;
    float savedActorZ = 0;
    double reachT = 0;
    float hitMc = 0;
    bool arrowOk = false, arrowFired = false, arrowSummoned = false;
    int arrowStep = 0;
    uint32_t arrowFlags = 0, arrowWeapon = 0;

    while (!g_quit && phase != Done && phase != Failed)
    {
        const double t = GetTickCount64() / 1000.0 - t0;
        if (t > timeout)
        {
            WLOG("combat test: TIMEOUT in phase %s", names[phase]);
            fail("timeout");
            phase = Failed;
            break;
        }
        const control::Frame fr = control::OnFrame(0, 0.0f, 0.0f);
        const auto st = host::GetStatus();
        const bool inWorld = st.mcAlive && st.haveMcState && (st.mcFlags & proto::kMcInWorld);
        const bool dead = st.haveMcState && (st.mcFlags & proto::kMcDead);

        // ---- the script's ~30 Hz step: Geralt follows Minecraft, host tick, ground samples ----
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
            const int bx = static_cast<int>(std::floor(cx)), by = static_cast<int>(std::floor(cy));
            for (int n = 0; n < 160; ++n)
            {
                const int idx = scanIndex++ % (33 * 33);
                char item[64];
                std::snprintf(item, sizeof(item), "%d,%d,%.3f;", bx + idx % 33 - 16, by + idx / 33 - 16, ground);
                batch += item;
            }
            host::PushHeights(batch);
            host::Tick(geraltX, geraltY, geraltZ, 180.0f - st.mcYaw, -st.mcPitch, host::kTickInGame);
        }
        // ---- the script's 10 Hz actor push (witchercraft_combat.ws PushActors) ----
        if (havePublishedActor && t - lastActors >= 0.1)
        {
            lastActors = t;
            char rec[160];
            std::snprintf(rec, sizeof(rec), "%d,%.2f,%.2f,%.2f,%.1f,0.60,1.80,1.000,9,12,Bandit;", actorId, actorX, actorY, actorZ, actorHeading);
            combat::SetActors(rec);
        }

        // Events the script would pop.
        combat::Event ev;
        while (combat::Pop(ev))
        {
            if (ev.type == proto::kEvHitActor)
            {
                WLOG("combat test: kEvHitActor id %d, %.2f Minecraft damage -> %.1f %% of max health, flags %u, weapon %u, knockback %.2f heading %.0f",
                     ev.id, ev.y, ev.x * 100.0f, ev.flags, ev.weapon, ev.z, ev.w);
                if (phase == Attack && !hitOk)
                {
                    hitOk = ev.id == actorId && ev.y > 5.0f && ev.weapon == proto::kWeaponBlade;
                    hitMc = ev.y;
                    WLOG("combat test: hit -> %s (id %s, damage %.2f, weapon %u)", hitOk ? "OK" : "BAD", ev.id == actorId ? "matches" : "WRONG", ev.y,
                         ev.weapon);
                    if (!hitOk)
                        fail("hit");
                    phase = Arrow;
                    phaseT = t;
                }
                else if (phase == Arrow && arrowFired && !arrowOk && ev.id == actorId)
                {
                    arrowFlags = ev.flags;
                    arrowWeapon = ev.weapon;
                    arrowOk = (ev.flags & proto::kHitProjectile) && ev.weapon == proto::kWeaponArrow && ev.y > 0.0f;
                    WLOG("combat test: ARROW: %s hit -> %s (flags 0x%X: projectile %s, weapon %u)", arrowSummoned ? "summoned arrow" : "crossbow bolt",
                         arrowOk ? "OK" : "BAD", ev.flags, (ev.flags & proto::kHitProjectile) ? "yes" : "NO", ev.weapon);
                }
            }
            else if (ev.type == proto::kEvPlayerDied)
            {
                WLOG("combat test: kEvPlayerDied (killer %d)", ev.id);
                if (phase == Lethal)
                {
                    diedOk = ev.id == actorId;
                    WLOG("combat test: death event -> %s (killer %s)", diedOk ? "OK" : "BAD", ev.id == actorId ? "is our actor" : "WRONG");
                    if (!diedOk)
                        fail("death killer");
                    phase = Respawn;
                    phaseT = t;
                }
            }
        }

        switch (phase)
        {
        case WaitDriven:
            if (st.driven && inWorld && st.teleportAck == st.teleportSeq && !st.gateHolding && fr.eyeValid)
            {
                WLOG("combat test: Minecraft drives (t=%.1f s); setting up Steve", t);
                for (const char* c : {"gamemode survival", "difficulty normal", "gamerule immediate_respawn true",
                                      "effect clear @s", "effect give @s minecraft:instant_health 1 5",
                                      "item replace entity @s weapon.mainhand with minecraft:diamond_sword"})
                    buildmode::QueueCommand(c);
                phase = Setup;
                phaseT = t;
            }
            break;
        case Setup:
            if (t - phaseT > 2.0 && buildmode::CommandsPending() == 0 && !(st.mcFlags & proto::kMcScreenOpen))
            {
                // Look level, and put the actor 2.5 m ahead of Steve, facing him.
                float yaw = 0, pitch = 0;
                control::GetLook(yaw, pitch);
                control::SetLook(yaw, 0.0f);
                const double r = yaw * 3.14159265358979 / 180.0;
                const double fx = -std::sin(r), fz = std::cos(r); // MC forward
                actorX = static_cast<float>(st.mcX + fx * 2.5);
                actorY = static_cast<float>(-(st.mcZ + fz * 2.5));
                actorZ = static_cast<float>(st.mcY);
                actorHeading = -yaw; // W3 heading of MC yaw + 180 (facing Steve): 180 - (yaw + 180)
                havePublishedActor = true;
                WLOG("combat test: actor %d published at W3 (%.2f, %.2f, %.2f), Steve at W3 (%.2f, %.2f, %.2f) yaw %.1f", actorId, actorX, actorY, actorZ,
                     st.mcX, -st.mcZ, st.mcY, yaw);
                phase = Spawn;
                phaseT = t;
            }
            break;
        case Spawn:
            if (t - phaseT > 2.5)
            {
                WLOG("combat test: ATTACK: left click (table: %llu actors published)", static_cast<unsigned long long>(combat::GetStats().actorsPublished));
                buildmode::QueueEvent({proto::kInMouseButton, 1, 1, 0, 0});
                ++clicks;
                phase = Attack;
                phaseT = t;
            }
            break;
        case Attack:
            if (t - phaseT > 0.1 && t - phaseT < 0.15)
                buildmode::QueueEvent({proto::kInMouseButton, 1, 0, 0, 0});
            if (t - phaseT > 3.0)
            {
                if (clicks < 3)
                {
                    WLOG("combat test: no hit yet; clicking again");
                    buildmode::QueueEvent({proto::kInMouseButton, 1, 1, 0, 0});
                    ++clicks;
                    phaseT = t;
                }
                else
                {
                    fail("no kEvHitActor after 3 clicks");
                    phase = Arrow;
                    phaseT = t;
                }
            }
            break;
        case Arrow:
            // A charged crossbow, aimed at the actor's chest (Steve may have been pushed a little).
            if (arrowStep == 0 && t - phaseT > 1.0 && buildmode::CommandsPending() == 0)
            {
                buildmode::QueueCommand("item replace entity @s weapon.mainhand with minecraft:crossbow[minecraft:charged_projectiles=[{id:\"minecraft:arrow\",count:1}]]");
                arrowStep = 1;
                phaseT = t;
            }
            else if (arrowStep == 1 && t - phaseT > 1.0 && buildmode::CommandsPending() == 0)
            {
                const double dx = actorX - st.mcX, dz = -actorY - st.mcZ;
                const double dist = std::sqrt(dx * dx + dz * dz);
                const float yaw = static_cast<float>(std::atan2(-dx, dz) * 57.29577951308232);
                const float pitch = static_cast<float>(std::atan2(1.62 - 1.1, dist) * 57.29577951308232); // eye -> chest
                control::SetLook(yaw, pitch);
                WLOG("combat test: ARROW: right click with a charged crossbow (actor %.2f m away, yaw %.1f pitch %.1f)", dist, yaw, pitch);
                buildmode::QueueEvent({proto::kInMouseButton, 3, 1, 0, 0});
                arrowFired = true;
                arrowStep = 2;
                phaseT = t;
            }
            else if (arrowStep == 2 && t - phaseT > 0.15)
            {
                buildmode::QueueEvent({proto::kInMouseButton, 3, 0, 0, 0});
                arrowStep = 3;
            }
            else if (arrowStep == 3 && !arrowOk && t - phaseT > 3.0)
            {
                // The crossbow didn't hit (item syntax, aim): a plain arrow flying into the actor is the same hit path.
                const double dx = actorX - st.mcX, dz = -actorY - st.mcZ;
                const double len = std::max(0.1, std::sqrt(dx * dx + dz * dz));
                char cmd[200];
                std::snprintf(cmd, sizeof(cmd), "summon minecraft:arrow %.2f %.2f %.2f {Motion:[%.3f,0.0,%.3f]}", actorX - dx / len * 1.4, actorZ + 1.1,
                              -actorY - dz / len * 1.4, dx / len * 1.6, dz / len * 1.6);
                buildmode::QueueCommand(cmd);
                WLOG("combat test: ARROW: no crossbow hit; /%s", cmd);
                arrowSummoned = true;
                arrowStep = 4;
                phaseT = t;
            }
            if (arrowOk || (arrowStep == 4 && t - phaseT > 3.0))
            {
                if (!arrowOk)
                    fail("arrow: no projectile hit");
                buildmode::QueueCommand("item replace entity @s weapon.mainhand with minecraft:diamond_sword");
                float yaw = 0, pitch = 0;
                control::GetLook(yaw, pitch);
                control::SetLook(yaw, 0.0f);
                phase = Hurt;
                phaseT = t;
            }
            break;
        case Hurt:
            if (t - phaseT > 1.0 && !hurtSent)
            {
                hurtSent = true;
                ReadFrom(logPath, 0, &logFrom);
                // 300 of Geralt's 3000 = 10 % -> 20 * 10 % * CombatHurtScale (1.5) = 3 Minecraft damage.
                const float mc = combat::Hurt(proto::kHurtMelee, 300.0f, 3000.0f, actorId, 0);
                WLOG("combat test: HURT: W3 melee hit 300 / 3000 from actor %d -> %.2f Minecraft damage", actorId, mc);
            }
            if (hurtSent && t - phaseT > 3.5)
            {
                float mc = 0, before = 0, after = 0;
                const bool found = FindHurtLine(ReadFrom(logPath, logFrom, nullptr), mc, before, after);
                // Minecraft applies its own armour to mob attacks (the persistent test world's Steve may wear
                // some), so only require that the 3.0 arrived and health really dropped.
                hurtOk = found && std::fabs(mc - 3.0f) < 0.05f && after < before - 0.5f;
                WLOG("combat test: Minecraft log %s: Steve took %.2f, health %.1f -> %.1f -> %s", found ? "has the hurt line" : "has NO hurt line", mc, before,
                     after, hurtOk ? "OK" : "BAD");
                if (!hurtOk)
                    fail("hurt");
                savedActorZ = actorZ;
                actorZ -= 5.0f;
                WLOG("combat test: REACH: the bandit moves 5 m below Steve");
                phase = Reach;
                phaseT = t;
            }
            break;
        case Reach:
            if (t - phaseT > 1.5 && !reachSent)
            {
                reachSent = true;
                reachT = t;
                ReadFrom(logPath, 0, &logFrom);
                combat::Hurt(proto::kHurtMelee, 300.0f, 3000.0f, actorId, 0);
                WLOG("combat test: REACH: W3 melee hit 300 / 3000 from the bandit below");
            }
            if (reachSent && t - reachT > 2.5)
            {
                const std::string log = ReadFrom(logPath, logFrom, nullptr);
                float mc = 0, before = 0, after = 0;
                const bool hurtLine = FindHurtLine(log, mc, before, after);
                reachOk = log.find("out of reach") != std::string::npos && !hurtLine;
                WLOG("combat test: REACH: Minecraft log %s, %s -> %s", log.find("out of reach") != std::string::npos ? "says out of reach" : "has NO out-of-reach line",
                     hurtLine ? "but Steve WAS hurt" : "Steve not hurt", reachOk ? "OK" : "BAD");
                if (!reachOk)
                    fail("reach");
                actorZ = savedActorZ;
                phase = ReachBack;
                phaseT = t;
            }
            break;
        case ReachBack:
            if (t - phaseT > 1.5)
            {
                const float lethal = combat::Hurt(proto::kHurtMelee, 30000.0f, 3000.0f, actorId, proto::kHurtPowerAttack);
                WLOG("combat test: LETHAL: power attack -> %.0f Minecraft damage", lethal);
                phase = Lethal;
                phaseT = t;
            }
            break;
        case Lethal:
            if (t - phaseT > 6.0)
            {
                fail("no kEvPlayerDied");
                phase = Respawn;
                phaseT = t;
            }
            break;
        case Respawn:
            if (!dead && t - phaseT > 1.0 && inWorld)
            {
                respawnOk = true;
                WLOG("combat test: Steve respawned (t+%.1f s) at W3 (%.2f, %.2f, %.2f)", t - phaseT, st.mcX, -st.mcZ, st.mcY);
            }
            if (respawnOk || t - phaseT > 10.0)
            {
                if (!respawnOk)
                    WLOG("combat test: Steve still dead after 10 s (immediate-respawn gamerule not accepted?) - informational");
                ReadFrom(logPath, 0, &logFrom);
                for (int i = 0; i < 3; ++i)
                    combat::Hurt(proto::kHurtMagic, 50.0f, 3000.0f, actorId, 0); // 3 x 0.5 -> one 1.5 after 0.5 s
                phase = Dot;
                phaseT = t;
            }
            break;
        case Dot:
            if (t - phaseT > 3.0)
            {
                float mc = 0, before = 0, after = 0;
                const bool found = FindHurtLine(ReadFrom(logPath, logFrom, nullptr), mc, before, after);
                dotOk = found && std::fabs(mc - 1.5f) < 0.05f;
                WLOG("combat test: batched magic damage: %s %.2f (expect one 1.50) -> %s", found ? "got" : "none", mc,
                     dotOk ? "OK" : (respawnOk ? "BAD" : "skipped (dead)"));
                phase = Done;
            }
            break;
        default:
            break;
        }
        if (t - lastPrint >= 3.0)
        {
            lastPrint = t;
            WLOG("t=%5.1fs %s | %s", t, names[phase], host::StatusLine().c_str());
        }
        Sleep(16);
    }
    combat::SetActors("");
    for (int i = 0; i < 20; ++i)
    {
        control::OnFrame(0, 0.0f, 0.0f);
        host::Tick(geraltX, geraltY, geraltZ, 0.0f, 0.0f, host::kTickInGame);
        Sleep(33);
    }
    const auto cs = combat::GetStats();
    const bool pass = fails.empty() && hitOk && arrowOk && hurtOk && reachOk && diedOk;
    WLOG("combat test: %s (hit %d [%.2f], arrow %d [%s, flags 0x%X weapon %u], hurt %d, reach %d, died %d, respawn %d, dot %d) %s; combat stats: tables %llu hits %llu hurts %llu deaths %llu",
         pass ? "PASS" : "FAIL", hitOk, hitMc, arrowOk, arrowSummoned ? "summoned" : "crossbow", arrowFlags, arrowWeapon, hurtOk, reachOk, diedOk, respawnOk, dotOk, fails.c_str(), static_cast<unsigned long long>(cs.tablesWritten),
         static_cast<unsigned long long>(cs.hits), static_cast<unsigned long long>(cs.hurtsSent), static_cast<unsigned long long>(cs.deaths));
    host::Stop();
    return pass ? 0 : 2;
}
} // namespace

// ---- M5c offline test: the W3 interact key (G) and Minecraft's third person (F5) --------------
//
//   fake_host.exe interact [timeoutSeconds=240]
//
// Minecraft mode on flat ground (as the control test). Raw keyboard events go through exactly the
// path W3's GetRawInputData calls take (control::TestRawInput):
//   - G while Minecraft drives: must NOT be forwarded to Minecraft, must come out as W3's
//     interaction key (E: scan 0x12, VK 'E', same make/break) and count one press for the script;
//     a duplicate fetch of the same event is remapped too; in Witcher mode G is left alone.
//   - E: forwarded to Minecraft (its inventory opens: kMcScreenOpen) and blanked for W3; with the
//     screen open G is typing (forwarded, not remapped); E closes it again.
//   - F5 -> cameraMode 1: the camera (interpolated AND the overlay frame's pose, if the world layer
//     publishes one) sits behind the feet along the player's look; Geralt's heading stays the
//     player's. Looking up 60 deg: Minecraft's zoom collision pulls the camera in against the
//     ground and the W3 eye stays above it. F5 -> mode 2: the camera is in front, looking back.
//     F5 -> mode 0: back at the eye.
// Exit code 0 = PASS.
namespace
{
bool NewestOverlayPose(wc::control::OverlayPose& out)
{
    namespace proto = skycraft::proto;
    uint8_t* base = wc::host::MappingBase();
    if (!base)
        return false;
    const proto::OverlaySlotHdr* best = nullptr;
    proto::OverlaySlotHdr copy{};
    for (uint32_t i = 0; i < proto::kOverlaySlots; ++i)
    {
        proto::OverlaySlotHdr h;
        std::memcpy(&h, base + proto::kOffOverlaySlotHdr + i * sizeof(proto::OverlaySlotHdr), sizeof(h));
        if ((h.flags & proto::kOverlayHasPose) && (!best || h.frameId > copy.frameId))
        {
            copy = h;
            best = &copy;
        }
    }
    if (!best)
        return false;
    out.frameId = copy.frameId;
    out.takenMs = GetTickCount64();
    out.eye[0] = copy.camEyeX, out.eye[1] = copy.camEyeY, out.eye[2] = copy.camEyeZ;
    out.yaw = copy.camYaw, out.pitch = copy.camPitch, out.fovDeg = copy.camFovDeg, out.rollDeg = copy.camRollDeg;
    return true;
}

RAWINPUT MakeKey(uint16_t dik, bool up)
{
    RAWINPUT r{};
    r.header.dwType = RIM_TYPEKEYBOARD;
    r.header.dwSize = sizeof(RAWINPUT);
    r.data.keyboard.MakeCode = static_cast<USHORT>(dik & 0x7F);
    r.data.keyboard.Flags = static_cast<USHORT>((up ? RI_KEY_BREAK : RI_KEY_MAKE) | ((dik & 0x80) ? RI_KEY_E0 : 0));
    r.data.keyboard.VKey = static_cast<USHORT>(MapVirtualKeyW(dik & 0x7F, MAPVK_VSC_TO_VK_EX));
    r.data.keyboard.Message = up ? WM_KEYUP : WM_KEYDOWN;
    return r;
}

int RunInteractTest(int timeout)
{
    using namespace wc;
    namespace proto = skycraft::proto;
    const float ground = 64.0f;
    const uint64_t seed = (GetTickCount64() / 1000) % 200;
    const float startX = 5000.0f + static_cast<float>(seed) * 150.0f + 0.5f, startY = -900.5f;
    WLOG("interact test (M5c r4): start W3 (%.1f, %.1f, %.1f), timeout %d s", startX, startY, ground, timeout);
    control::SetMcMode(true);
    if (!host::Start())
        return 1;
    host::SetWorld("fake\\interact_test.w2w");

    std::string fails;
    auto fail = [&](const std::string& what) {
        WLOG("interact test: FAIL: %s", what.c_str());
        fails += what + "; ";
    };
    float geraltX = startX, geraltY = startY, geraltZ = ground;
    int scanIndex = 0;
    const double t0 = GetTickCount64() / 1000.0;
    double lastStep = -1;
    control::Frame fr;
    host::Status st;
    // One script frame (+ the 30 Hz step when due), like witchercraft.ws / witchercraft_control.ws.
    auto pump = [&]() {
        fr = control::OnFrame(0, 0.0f, 0.0f);
        st = host::GetStatus();
        const double t = GetTickCount64() / 1000.0 - t0;
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
            const int bx = static_cast<int>(std::floor(cx)), by = static_cast<int>(std::floor(cy));
            for (int n = 0; n < 160; ++n)
            {
                const int idx = scanIndex++ % (33 * 33);
                char item[64];
                std::snprintf(item, sizeof(item), "%d,%d,%.3f;", bx + idx % 33 - 16, by + idx / 33 - 16, ground);
                batch += item;
            }
            host::PushHeights(batch);
            host::Tick(geraltX, geraltY, geraltZ, fr.bodyHeadingDeg, fr.lookPitchUpDeg, host::kTickInGame);
        }
        Sleep(16);
        return !g_quit && t < timeout;
    };
    auto wait = [&](double seconds) {
        const double until = GetTickCount64() / 1000.0 + seconds;
        while (GetTickCount64() / 1000.0 < until)
            if (!pump())
                return false;
        return true;
    };
    auto waitFor = [&](double seconds, auto cond) {
        const double until = GetTickCount64() / 1000.0 + seconds;
        while (GetTickCount64() / 1000.0 < until)
        {
            if (!pump())
                return false;
            if (cond())
                return true;
        }
        return false;
    };
    uintptr_t handle = 0x5000;
    auto key = [&](uint16_t dik, bool up) {
        RAWINPUT r = MakeKey(dik, up);
        control::TestRawInput(&r, reinterpret_cast<void*>(++handle));
        return r;
    };
    auto drivenNow = [&]() { return st.driven && fr.engaged && fr.eyeValid && fr.arrived && !st.gateHolding; };

    bool gOk = false, dupOk = false, witcherOk = false, eOk = false, typeOk = false;
    bool f5Ok = false, poseOk = false, zoomOk = false, frontOk = false, backOk = false;
    bool poseSeen = false;
    do
    {
        if (!waitFor(timeout, drivenNow))
        {
            fail("Minecraft never drove");
            break;
        }
        WLOG("interact test: Minecraft drives at W3 (%.2f, %.2f, %.2f)", fr.feet[0], fr.feet[1], fr.feet[2]);
        wait(1.0);
        control::ConsumeInteract();

        // ---- G in Minecraft mode ----
        auto in0 = control::Inputs();
        RAWINPUT down = key(0x22, false);
        const auto& kd = down.data.keyboard;
        RAWINPUT dup = MakeKey(0x22, false);
        control::TestRawInput(&dup, reinterpret_cast<void*>(handle)); // W3 fetching the same event again
        RAWINPUT upEv = key(0x22, true);
        const auto& ku = upEv.data.keyboard;
        auto in1 = control::Inputs();
        const auto poll = control::ConsumeInteract();
        gOk = down.header.dwType == RIM_TYPEKEYBOARD && kd.MakeCode == 0x12 && kd.VKey == 'E' && !(kd.Flags & RI_KEY_BREAK) &&
              upEv.header.dwType == RIM_TYPEKEYBOARD && ku.MakeCode == 0x12 && (ku.Flags & RI_KEY_BREAK) && in1.keysSent == in0.keysSent &&
              poll.presses == 1 && poll.givenToW3 == 1 && !poll.held;
        dupOk = dup.header.dwType == RIM_TYPEKEYBOARD && dup.data.keyboard.MakeCode == 0x12;
        WLOG("interact test: G down -> W3 sees scan 0x%02X VK '%c' type %u; G up -> scan 0x%02X break %d; duplicate fetch -> 0x%02X; keys "
             "forwarded to Minecraft +%llu; poll presses %u given to W3 %u -> %s",
             kd.MakeCode, kd.VKey, down.header.dwType, ku.MakeCode, (ku.Flags & RI_KEY_BREAK) ? 1 : 0, dup.data.keyboard.MakeCode,
             static_cast<unsigned long long>(in1.keysSent - in0.keysSent), poll.presses, poll.givenToW3, gOk && dupOk ? "OK" : "BAD");
        if (!gOk)
            fail("G path");
        if (!dupOk)
            fail("G duplicate fetch");

        // ---- Witcher mode: G is W3's own key ----
        control::SetMcMode(false);
        wait(0.3);
        RAWINPUT wg = key(0x22, false);
        key(0x22, true);
        const auto wpoll = control::ConsumeInteract();
        witcherOk = wg.header.dwType == RIM_TYPEKEYBOARD && wg.data.keyboard.MakeCode == 0x22 && wpoll.presses == 0;
        WLOG("interact test: Witcher mode: G -> scan 0x%02X, presses %u -> %s", wg.data.keyboard.MakeCode, wpoll.presses, witcherOk ? "OK" : "BAD");
        if (!witcherOk)
            fail("Witcher-mode G");
        control::SetMcMode(true);
        if (!waitFor(30, drivenNow))
        {
            fail("no re-engage after the mode toggle");
            break;
        }
        wait(0.5);

        // ---- E: Minecraft's inventory; then G is typing ----
        in0 = control::Inputs();
        RAWINPUT e1 = key(0x12, false);
        key(0x12, true);
        const bool opened = waitFor(3.0, [&] { return (st.mcFlags & proto::kMcScreenOpen) != 0; });
        in1 = control::Inputs();
        eOk = e1.header.dwType == RIM_TYPEHID && in1.keysSent == in0.keysSent + 1 && opened;
        WLOG("interact test: E -> W3 sees type %u (HID = blanked), forwarded +%llu, Minecraft screen open %s -> %s", e1.header.dwType,
             static_cast<unsigned long long>(in1.keysSent - in0.keysSent), opened ? "yes" : "no", eOk ? "OK" : "BAD");
        if (!eOk)
            fail("E path");
        wait(0.3);
        in0 = control::Inputs();
        RAWINPUT tg = key(0x22, false);
        key(0x22, true);
        in1 = control::Inputs();
        const auto tpoll = control::ConsumeInteract();
        typeOk = tg.header.dwType == RIM_TYPEHID && in1.keysSent == in0.keysSent + 1 && tpoll.presses == 0;
        WLOG("interact test: G with a Minecraft screen open -> type %u, forwarded +%llu, presses %u -> %s", tg.header.dwType,
             static_cast<unsigned long long>(in1.keysSent - in0.keysSent), tpoll.presses, typeOk ? "OK (typing)" : "BAD");
        if (!typeOk)
            fail("G with a screen open");
        key(0x12, false);
        key(0x12, true);
        if (!waitFor(3.0, [&] { return (st.mcFlags & proto::kMcScreenOpen) == 0; }))
            fail("inventory did not close");
        wait(0.5);

        // ---- F5: third person behind ----
        control::SetPoseProvider(nullptr);
        control::SetUsePose(false);
        key(0x3F, false);
        key(0x3F, true);
        proto::McState mc{};
        const bool mode1 = waitFor(3.0, [&] { return host::ReadMc(mc) && mc.cameraMode == 1 && mc.cameraDistance > 2.0f; });
        wait(0.6); // zoom eases out
        auto rel = [&](double& along, double& side, double& up) {
            const double h = fr.bodyHeadingDeg * 3.14159265358979 / 180.0;
            const double fx = -std::sin(h), fy = std::cos(h);
            const double dx = fr.eye[0] - fr.feet[0], dy = fr.eye[1] - fr.feet[1];
            along = dx * fx + dy * fy;
            side = dx * fy - dy * fx;
            up = fr.eye[2] - fr.feet[2];
        };
        double along, side, up;
        rel(along, side, up);
        host::ReadMc(mc);
        f5Ok = mode1 && fr.cameraMode == 1 && along < -2.5 && std::fabs(side) < 0.5 && up > 1.0 &&
               std::fabs(std::remainder(fr.headingDeg - fr.bodyHeadingDeg, 360.0)) < 1.0;
        WLOG("interact test: F5 -> cameraMode %u distance %.2f; interpolated camera vs feet: %.2f m along the look, %.2f m to the side, %.2f m "
             "up; camera heading %.1f body %.1f -> %s",
             mc.cameraMode, mc.cameraDistance, along, side, up, fr.headingDeg, fr.bodyHeadingDeg, f5Ok ? "OK (behind)" : "BAD");
        if (!f5Ok)
            fail("F5 behind");

        // the overlay frame's pose (world layer): what the game uses by default
        control::SetPoseProvider(&NewestOverlayPose);
        control::SetUsePose(true);
        poseSeen = waitFor(3.0, [&] { return fr.fromPose; });
        if (poseSeen)
        {
            wait(0.3);
            rel(along, side, up);
            poseOk = fr.fromPose && along < -2.5 && std::fabs(side) < 0.5 && up > 1.0;
            WLOG("interact test: overlay pose camera vs feet: %.2f along, %.2f side, %.2f up -> %s", along, side, up, poseOk ? "OK (behind)" : "BAD");
            if (!poseOk)
                fail("pose behind");
        }
        else
        {
            poseOk = true;
            WLOG("interact test: no overlay frame with a camera pose (HUD-only Minecraft?) - pose check skipped");
        }

        // zoom collision: look up 60 deg, the camera would go 2+ m under the ground
        float yaw, pitch;
        control::GetLook(yaw, pitch);
        control::SetLook(yaw, -60.0f);
        wait(1.0);
        host::ReadMc(mc);
        rel(along, side, up);
        zoomOk = mc.cameraDistance < 3.0f && fr.eye[2] > ground + 0.02;
        WLOG("interact test: looking up 60 deg in F5: Minecraft camera distance %.2f (collision), W3 eye z %.2f (ground %.1f) -> %s",
             mc.cameraDistance, fr.eye[2], ground, zoomOk ? "OK" : "BAD");
        if (!zoomOk)
            fail("zoom collision");
        control::SetLook(yaw, 0.0f);
        wait(0.8);

        // ---- F5: front, looking back ----
        key(0x3F, false);
        key(0x3F, true);
        const bool mode2 = waitFor(3.0, [&] { return host::ReadMc(mc) && mc.cameraMode == 2; });
        wait(0.8);
        rel(along, side, up);
        frontOk = mode2 && along > 2.5 && std::fabs(side) < 0.5 && std::fabs(std::remainder(fr.headingDeg - fr.bodyHeadingDeg - 180.0, 360.0)) < 2.0 &&
                  std::fabs(std::remainder(fr.bodyHeadingDeg - (180.0 - yaw), 360.0)) < 1.0;
        WLOG("interact test: F5 again -> cameraMode %u: camera %.2f m along the look (%s), %.2f side; camera heading %.1f, body %.1f -> %s", mc.cameraMode,
             along, fr.fromPose ? "pose" : "interpolated", side, fr.headingDeg, fr.bodyHeadingDeg, frontOk ? "OK (in front, looking back)" : "BAD");
        if (!frontOk)
            fail("F5 front");

        // ---- F5: first person again ----
        key(0x3F, false);
        key(0x3F, true);
        const bool mode0 = waitFor(3.0, [&] { return host::ReadMc(mc) && mc.cameraMode == 0; });
        wait(0.5);
        rel(along, side, up);
        backOk = mode0 && std::fabs(along) < 0.4 && std::fabs(side) < 0.4 && std::fabs(up - fr.eyeHeight) < 0.3;
        WLOG("interact test: F5 again -> cameraMode %u: camera %.2f along %.2f side %.2f up (eye height %.2f) -> %s", mc.cameraMode, along, side, up,
             fr.eyeHeight, backOk ? "OK (first person)" : "BAD");
        if (!backOk)
            fail("F5 back to first person");
    } while (false);

    control::SetPoseProvider(nullptr);
    for (int i = 0; i < 20; ++i)
        pump();
    const bool pass = fails.empty() && gOk && dupOk && witcherOk && eOk && typeOk && f5Ok && poseOk && zoomOk && frontOk && backOk;
    WLOG("interact test: %s (G %d dup %d witcher %d E %d typing %d | F5 behind %d pose %d%s zoom %d front %d first %d) %s; %s", pass ? "PASS" : "FAIL", gOk,
         dupOk, witcherOk, eOk, typeOk, f5Ok, poseOk, poseSeen ? "" : " (skipped)", zoomOk, frontOk, backOk, fails.c_str(),
         host::StatusLine().c_str());
    host::Stop();
    return pass ? 0 : 2;
}

// M6a mode identity, against the real SkyCraft dev client:
//   Minecraft mode: Esc -> Minecraft's pause menu (kInOpenMenu; W3 sees nothing, not even the
//   release); Esc with that screen open -> Minecraft's key (closes it); M while Steve is in the air
//   (Geralt grounded) -> the Witcher's map over Minecraft mode: no mode switch, Minecraft paused, the
//   script's pause doesn't make the host re-place Steve, and Minecraft resumes by itself when the
//   panel ends; a panel the script never opens ends by itself; N -> Witcher mode + the
//   meditation request 0x31 (W3 never sees M or N: press, repeat and release are swallowed);
//   the overlay flag (Minecraft mode yes, suspended no, Witcher mode no, Witcher + F7 build yes);
//   Witcher mode: Esc untouched for W3; F8-style toggle back -> Minecraft drives again.
int RunIdentityTest(int timeout)
{
    using namespace wc;
    namespace proto = skycraft::proto;
    const float ground = 64.0f;
    const uint64_t seed = (GetTickCount64() / 1000) % 200;
    const float startX = -7000.0f - static_cast<float>(seed) * 150.0f + 0.5f, startY = 1200.5f;
    WLOG("identity test (M6a): start W3 (%.1f, %.1f, %.1f), timeout %d s", startX, startY, ground, timeout);
    control::SetMcMode(true);
    if (!host::Start())
        return 1;
    host::SetWorld("fake\\identity_test.w2w");

    std::string fails;
    auto fail = [&](const std::string& what) {
        WLOG("identity test: FAIL: %s", what.c_str());
        fails += what + "; ";
    };
    float geraltX = startX, geraltY = startY, geraltZ = ground;
    int scanIndex = 0;
    const double t0 = GetTickCount64() / 1000.0;
    double lastStep = -1;
    control::Frame fr;
    host::Status st;
    uint32_t extraFlags = 0;
    bool groundGeralt = false; // like the script's FollowBody: Geralt on the ground under a Steve in the air
    auto pump = [&]() {
        // like witchercraft_control.ws: 4 = Geralt kept on the ground while Minecraft drives
        fr = control::OnFrame(extraFlags | (fr.engaged ? static_cast<uint32_t>(control::kFrameGrounded) : 0u), 0.0f, 0.0f);
        st = host::GetStatus();
        const double t = GetTickCount64() / 1000.0 - t0;
        if (t - lastStep >= 0.033)
        {
            lastStep = t;
            if (fr.engaged && fr.eyeValid && fr.arrived)
            {
                geraltX = static_cast<float>(fr.feet[0]);
                geraltY = static_cast<float>(fr.feet[1]);
                geraltZ = groundGeralt ? ground : static_cast<float>(fr.feet[2]);
            }
            float cx = geraltX, cy = geraltY, cz = geraltZ;
            host::SampleCentre(cx, cy, cz);
            std::string batch;
            const int bx = static_cast<int>(std::floor(cx)), by = static_cast<int>(std::floor(cy));
            for (int n = 0; n < 160; ++n)
            {
                const int idx = scanIndex++ % (33 * 33);
                char item[64];
                std::snprintf(item, sizeof(item), "%d,%d,%.3f;", bx + idx % 33 - 16, by + idx / 33 - 16, ground);
                batch += item;
            }
            host::PushHeights(batch);
            host::Tick(geraltX, geraltY, geraltZ, fr.bodyHeadingDeg, fr.lookPitchUpDeg, host::kTickInGame);
        }
        Sleep(16);
        return !g_quit && t < timeout;
    };
    auto wait = [&](double seconds) {
        const double until = GetTickCount64() / 1000.0 + seconds;
        while (GetTickCount64() / 1000.0 < until)
            if (!pump())
                return false;
        return true;
    };
    auto waitFor = [&](double seconds, auto cond) {
        const double until = GetTickCount64() / 1000.0 + seconds;
        while (GetTickCount64() / 1000.0 < until)
        {
            if (!pump())
                return false;
            if (cond())
                return true;
        }
        return false;
    };
    uintptr_t handle = 0x7000;
    auto key = [&](uint16_t dik, bool up) {
        RAWINPUT r = MakeKey(dik, up);
        control::TestRawInput(&r, reinterpret_cast<void*>(++handle));
        return r;
    };
    auto hidden = [](const RAWINPUT& r) { return r.header.dwType == RIM_TYPEHID; };
    auto drivenNow = [&]() { return st.driven && fr.engaged && fr.eyeValid && fr.arrived && !st.gateHolding; };

    bool escOk = false, escCloseOk = false, panelOk = false, panelNetOk = false, medOk = false, overlayOk = false, witcherEscOk = false, backOk = false,
         groundedOk = false;
    do
    {
        if (!waitFor(timeout, drivenNow))
        {
            fail("Minecraft never drove");
            break;
        }
        WLOG("identity test: Minecraft drives at W3 (%.2f, %.2f, %.2f)", fr.feet[0], fr.feet[1], fr.feet[2]);
        wait(1.0);
        groundedOk = control::GeraltGrounded();
        WLOG("identity test: grounded flag while Minecraft drives: %d -> %s", groundedOk ? 1 : 0, groundedOk ? "OK" : "BAD");
        if (!groundedOk)
            fail("grounded flag");

        // ---- Esc in Minecraft mode: Minecraft's pause menu ----
        const bool ovMc = control::OverlayWanted();
        auto in0 = control::Inputs();
        RAWINPUT e1 = key(0x01, false);
        RAWINPUT e1r = key(0x01, false); // auto-repeat
        RAWINPUT e1u = key(0x01, true);
        const bool opened = waitFor(4.0, [&] { return (st.mcFlags & proto::kMcScreenOpen) != 0; });
        auto in1 = control::Inputs();
        escOk = hidden(e1) && hidden(e1r) && hidden(e1u) && in1.mcMenuOpens == in0.mcMenuOpens + 1 && in1.keysSent == in0.keysSent && opened;
        WLOG("identity test: Esc -> W3 sees types %u/%u/%u (HID = hidden), kInOpenMenu +%llu, keys +%llu, Minecraft screen open %s -> %s",
             e1.header.dwType, e1r.header.dwType, e1u.header.dwType, static_cast<unsigned long long>(in1.mcMenuOpens - in0.mcMenuOpens),
             static_cast<unsigned long long>(in1.keysSent - in0.keysSent), opened ? "yes" : "no", escOk ? "OK" : "BAD");
        if (!escOk)
            fail("Esc -> Minecraft menu");
        wait(0.5);

        // ---- Esc with the menu open: Minecraft's own key, closes it ----
        in0 = control::Inputs();
        RAWINPUT e2 = key(0x01, false);
        RAWINPUT e2u = key(0x01, true);
        const bool closed = waitFor(4.0, [&] { return (st.mcFlags & proto::kMcScreenOpen) == 0; });
        in1 = control::Inputs();
        escCloseOk = hidden(e2) && hidden(e2u) && in1.keysSent == in0.keysSent + 1 && in1.mcMenuOpens == in0.mcMenuOpens && closed;
        WLOG("identity test: Esc with the menu open -> forwarded +%llu, menu opens +%llu, screen closed %s -> %s",
             static_cast<unsigned long long>(in1.keysSent - in0.keysSent), static_cast<unsigned long long>(in1.mcMenuOpens - in0.mcMenuOpens),
             closed ? "yes" : "no", escCloseOk ? "OK" : "BAD");
        if (!escCloseOk)
            fail("Esc closes the Minecraft menu");
        wait(0.5);

        // ---- overlay flag: Minecraft mode yes, suspended (scene) no ----
        extraFlags = control::kFrameSuspend;
        pump();
        const bool ovSuspended = control::OverlayWanted();
        extraFlags = 0;
        pump();
        const bool ovBack = control::OverlayWanted();
        if (!waitFor(30, drivenNow))
        {
            fail("no re-engage after the suspend frame");
            break;
        }

        // ---- M as Steve, in the air: the Witcher's map opens OVER Minecraft mode ----
        // Steve rises (levitation) while Geralt stays on the ground under him, as the script keeps
        // him. M: no mode switch; input goes to The Witcher; Minecraft pauses (Steve keeps his place);
        // "the script" opens the map (PanelBegin) and is then paused by the menu for 4.6 s (no frames,
        // no ticks): the host must not see a load nor re-place Steve onto Geralt. The menu closes, the
        // script ends the panel, and Minecraft goes on by itself: still rising, no teleport, driving.
        groundGeralt = true;
        buildmode::QueueCommand("effect give @s minecraft:levitation 20 1 true");
        const bool rose = waitFor(30, [&] {
            return buildmode::CommandsPending() == 0 && !(st.mcFlags & proto::kMcScreenOpen) && st.mcY > ground + 6.0 && drivenNow();
        });
        WLOG("identity test: Steve in the air at MC y %.2f, Geralt kept on the ground at %.2f -> %s", st.mcY, geraltZ, rose ? "OK" : "BAD");
        if (!rose)
        {
            fail("Steve never got into the air");
            break;
        }
        const uint64_t tp0 = st.teleports, holds0 = st.gateHolds;
        control::ConsumeW3MenuRequest();
        in0 = control::Inputs();
        const double yKey = st.mcY;
        RAWINPUT m1 = key(0x32, false);
        const bool modeKept = control::McMode();
        const uint32_t ps1 = control::PanelState();
        const bool capOff = !control::Capturing();
        const bool ovOff = !control::OverlayWanted();
        RAWINPUT m1r = key(0x32, false); // auto-repeat
        RAWINPUT m1u = key(0x32, true);
        const uint32_t req = control::ConsumeW3MenuRequest();
        const uint32_t req2 = control::ConsumeW3MenuRequest();
        in1 = control::Inputs();
        // The script's next frame: it opens the map and reports where Geralt stands; the menu is up.
        control::PanelBegin(geraltX, geraltY, geraltZ);
        extraFlags = control::kFrameNoCapture;
        pump();
        pump();
        const bool keyOk = hidden(m1) && hidden(m1r) && hidden(m1u) && modeKept && (ps1 & 1) && capOff && ovOff && req == 0x32 && req2 == 0 &&
                           in1.w3Panels == in0.w3Panels + 1 && in1.w3MenuSwitch == in0.w3MenuSwitch && in1.keysSent == in0.keysSent;
        WLOG("identity test: M -> W3 sees types %u/%u/%u, Minecraft mode kept %d, panel state %u, capture off %d, overlay off %d, request 0x%02X then "
             "0x%02X, panels +%llu -> %s",
             m1.header.dwType, m1r.header.dwType, m1u.header.dwType, modeKept ? 1 : 0, ps1, capOff ? 1 : 0, ovOff ? 1 : 0, req, req2,
             static_cast<unsigned long long>(in1.w3Panels - in0.w3Panels), keyOk ? "OK" : "BAD");
        // The Witcher's menu pauses the script: no OnFrame, no host::Tick.
        Sleep(400);
        const host::Status p0 = host::GetStatus();
        double maxDev = 0.0;
        bool sawLoading = false, sawHold = false;
        for (int i = 0; i < 42; ++i)
        {
            Sleep(100);
            const host::Status s = host::GetStatus();
            maxDev = std::max(maxDev, std::sqrt((s.mcX - p0.mcX) * (s.mcX - p0.mcX) + (s.mcY - p0.mcY) * (s.mcY - p0.mcY) +
                                                (s.mcZ - p0.mcZ) * (s.mcZ - p0.mcZ)));
            sawLoading = sawLoading || (s.skyFlags & proto::kSkyLoading);
            sawHold = sawHold || s.gateHolding;
        }
        const host::Status p1 = host::GetStatus();
        const uint32_t psPaused = control::PanelState();
        const bool frozenOk = maxDev < 0.05 && !sawLoading && !sawHold && p1.teleports == tp0 && (psPaused & 7) == 7 && control::Inputs().panelReTicks > 0;
        WLOG("identity test: map open 4.6 s with the script paused: Minecraft's player moved %.3f m (%.2f m between the key and the pause), host "
             "loading %d, hold %d, teleports +%llu, re-ticks %llu, panel state %u -> %s",
             maxDev, p0.mcY - yKey, sawLoading ? 1 : 0, sawHold ? 1 : 0, static_cast<unsigned long long>(p1.teleports - tp0),
             static_cast<unsigned long long>(control::Inputs().panelReTicks), psPaused, frozenOk ? "OK" : "BAD");
        // The menu closes: the script runs again and ends the panel 3 frames later.
        extraFlags = 0;
        pump();
        pump();
        pump();
        control::PanelEnd("test: the menu closed");
        const bool resumed = waitFor(3.0, [&] { return st.mcY > p1.mcY + 0.5 && drivenNow(); });
        const bool noTp = st.teleports == tp0 && st.gateHolds == holds0;
        const double gapAfter = st.mcY - p1.mcY;
        panelOk = keyOk && frozenOk && resumed && noTp && control::McMode() && control::Capturing() && control::OverlayWanted() &&
                  !(control::PanelState() & 1);
        WLOG("identity test: map closed -> Minecraft mode %d, driven %d, capture %d, overlay %d, Steve rising again +%.2f m, teleports +%llu, holds +%llu "
             "-> %s",
             control::McMode() ? 1 : 0, drivenNow() ? 1 : 0, control::Capturing() ? 1 : 0, control::OverlayWanted() ? 1 : 0, gapAfter,
             static_cast<unsigned long long>(st.teleports - tp0), static_cast<unsigned long long>(st.gateHolds - holds0), panelOk ? "OK" : "BAD");
        if (!panelOk)
            fail("M -> the Witcher's map over Minecraft mode");

        // ---- safety net: a panel the script never opens ends by itself; Minecraft goes on ----
        RAWINPUT j1 = key(0x24, false);
        RAWINPUT j1u = key(0x24, true);
        const bool jOn = (control::PanelState() & 1) != 0;
        const bool jEnded = waitFor(4.0, [&] { return !(control::PanelState() & 1); }) && waitFor(4.0, drivenNow) && control::Capturing();
        const uint32_t jReq = control::ConsumeW3MenuRequest();
        panelNetOk = hidden(j1) && hidden(j1u) && jOn && jEnded && jReq == 0x24;
        WLOG("identity test: J with no script answering -> panel on %d, ended by itself %d, request 0x%02X -> %s", jOn ? 1 : 0, jEnded ? 1 : 0, jReq,
             panelNetOk ? "OK" : "BAD");
        if (!panelNetOk)
            fail("panel safety net");

        // back on the ground
        groundGeralt = false;
        buildmode::QueueCommand("effect clear @s minecraft:levitation");
        buildmode::QueueCommand("tp @s ~ 64.2 ~");
        waitFor(20, [&] { return buildmode::CommandsPending() == 0 && (st.mcFlags & proto::kMcOnGround) && st.mcY < ground + 1.0 && drivenNow(); });
        wait(0.5);

        // ---- N (meditation) still switches to Witcher mode: Geralt sits down while time passes ----
        control::ConsumeW3MenuRequest();
        in0 = control::Inputs();
        RAWINPUT n1 = key(0x31, false);
        const bool modeOff = !control::McMode();
        pump();
        RAWINPUT n1r = key(0x31, false); // auto-repeat, capture already off
        RAWINPUT n1u = key(0x31, true);
        const uint32_t nReq = control::ConsumeW3MenuRequest();
        const uint32_t nReq2 = control::ConsumeW3MenuRequest();
        in1 = control::Inputs();
        wait(0.3);
        medOk = hidden(n1) && hidden(n1r) && hidden(n1u) && modeOff && nReq == 0x31 && nReq2 == 0 && in1.w3MenuSwitch == in0.w3MenuSwitch + 1 &&
                in1.keysSent == in0.keysSent && !fr.engaged && !(control::PanelState() & 1);
        WLOG("identity test: N -> W3 sees types %u/%u/%u, Minecraft mode off %d, request 0x%02X then 0x%02X, engaged now %d -> %s", n1.header.dwType,
             n1r.header.dwType, n1u.header.dwType, modeOff ? 1 : 0, nReq, nReq2, fr.engaged ? 1 : 0, medOk ? "OK" : "BAD");
        if (!medOk)
            fail("N -> Witcher mode + meditation");

        // ---- Witcher mode: overlay off; Esc is The Witcher's ----
        const bool ovWitcher = control::OverlayWanted();
        buildmode::SetEnabled(true);
        const bool ovBuild = control::OverlayWanted();
        buildmode::SetEnabled(false);
        const bool ovWitcher2 = control::OverlayWanted();
        overlayOk = ovMc && !ovSuspended && ovBack && !ovWitcher && ovBuild && !ovWitcher2;
        WLOG("identity test: overlay wanted: Minecraft mode %d, suspended %d, after %d, Witcher mode %d, Witcher + F7 build %d, build off %d -> %s",
             ovMc, ovSuspended, ovBack, ovWitcher, ovBuild, ovWitcher2, overlayOk ? "OK" : "BAD");
        if (!overlayOk)
            fail("overlay flag");
        RAWINPUT we = key(0x01, false);
        RAWINPUT weu = key(0x01, true);
        witcherEscOk = we.header.dwType == RIM_TYPEKEYBOARD && we.data.keyboard.MakeCode == 0x01 && weu.header.dwType == RIM_TYPEKEYBOARD &&
                       (weu.data.keyboard.Flags & RI_KEY_BREAK);
        WLOG("identity test: Witcher mode Esc -> W3 sees type %u scan 0x%02X -> %s", we.header.dwType, we.data.keyboard.MakeCode,
             witcherEscOk ? "OK (The Witcher's)" : "BAD");
        if (!witcherEscOk)
            fail("Witcher-mode Esc");

        // ---- back to Minecraft mode ----
        control::SetMcMode(true);
        backOk = waitFor(30, drivenNow) && control::OverlayWanted();
        WLOG("identity test: back to Minecraft mode -> driven %d -> %s", backOk ? 1 : 0, backOk ? "OK" : "BAD");
        if (!backOk)
            fail("re-engage");
    } while (false);

    for (int i = 0; i < 20; ++i)
        pump();
    const bool pass = fails.empty() && escOk && escCloseOk && panelOk && panelNetOk && medOk && overlayOk && witcherEscOk && backOk && groundedOk;
    WLOG("identity test: %s (Esc %d close %d panel %d panelNet %d meditation %d overlay %d witcherEsc %d back %d grounded %d) %s; %s",
         pass ? "PASS" : "FAIL", escOk, escCloseOk, panelOk, panelNetOk, medOk, overlayOk, witcherEscOk, backOk, groundedOk, fails.c_str(),
         host::StatusLine().c_str());
    host::Stop();
    return pass ? 0 : 2;
}
} // namespace

int RunWallsTest(int timeout);         // M5a, tools/fake_host_walls.cpp
int RunFlightTest(int timeout);        // M6b, tools/fake_host_walls.cpp (fast flight: no tunnelling)
int RunFeaturesTest(int timeout);      // WitcherCraft: Roach, torch light, boss bar (tools/fake_host_features.cpp)
int RunIdle(int seconds);              // keep Minecraft linked on flat ground (tools/fake_host_features.cpp)
int RunMobsTest(int timeout);          // WitcherCraft: Minecraft mobs fight the Witcher's enemies (tools/fake_host_mobs.cpp)
int RunTntTest(int timeout);           // WitcherCraft: TNT chain reactions on Witcher ground (tools/fake_host_tnt.cpp)
int RunBuildTest(int timeout);         // WitcherCraft: placing blocks on sloped Witcher ground, TNT (tools/fake_host_build.cpp)
int RunColScan(const wchar_t* dirArg); // M5a, tools/col_scan.cpp (offline PhysX signature check)

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "colscan") == 0)
    {
        wc::log::InitConsole();
        std::wstring dir;
        if (argc > 2)
            dir.assign(argv[2], argv[2] + std::strlen(argv[2]));
        return RunColScan(argc > 2 ? dir.c_str() : nullptr);
    }
    if (argc > 1 && std::strcmp(argv[1], "idle") == 0)
    {
        wc::log::InitConsole();
        return RunIdle(argc > 2 ? std::atoi(argv[2]) : 90);
    }
    if (argc > 1 && std::strcmp(argv[1], "build") == 0)
    {
        wc::log::InitConsole();
        return RunBuildTest(argc > 2 ? std::atoi(argv[2]) : 240);
    }
    if (argc > 1 && std::strcmp(argv[1], "tnt") == 0)
    {
        wc::log::InitConsole();
        return RunTntTest(argc > 2 ? std::atoi(argv[2]) : 240);
    }
    if (argc > 1 && std::strcmp(argv[1], "mobs") == 0)
    {
        wc::log::InitConsole();
        return RunMobsTest(argc > 2 ? std::atoi(argv[2]) : 240);
    }
    if (argc > 1 && std::strcmp(argv[1], "features") == 0)
    {
        wc::log::InitConsole();
        return RunFeaturesTest(argc > 2 ? std::atoi(argv[2]) : 180);
    }
    if (argc > 1 && std::strcmp(argv[1], "flight") == 0) // M6b
    {
        wc::log::InitConsole();
        return RunFlightTest(argc > 2 ? std::atoi(argv[2]) : 300);
    }
    if (argc > 1 && std::strcmp(argv[1], "walls") == 0)
    {
        wc::log::InitConsole();
        return RunWallsTest(argc > 2 ? std::atoi(argv[2]) : 300);
    }
    if (argc > 1 && std::strcmp(argv[1], "identity") == 0)
    {
        SetConsoleCtrlHandler(OnCtrl, TRUE);
        wc::log::InitConsole();
        return RunIdentityTest(argc > 2 ? std::atoi(argv[2]) : 240);
    }
    if (argc > 1 && std::strcmp(argv[1], "interact") == 0)
    {
        SetConsoleCtrlHandler(OnCtrl, TRUE);
        wc::log::InitConsole();
        return RunInteractTest(argc > 2 ? std::atoi(argv[2]) : 240);
    }
    if (argc > 1 && std::strcmp(argv[1], "combat") == 0)
    {
        SetConsoleCtrlHandler(OnCtrl, TRUE);
        wc::log::InitConsole();
        return RunCombatTest(argc > 2 ? std::atoi(argv[2]) : 300);
    }
    if (argc > 1 && std::strcmp(argv[1], "control") == 0)
    {
        SetConsoleCtrlHandler(OnCtrl, TRUE);
        wc::log::InitConsole();
        return RunControlTest(argc > 2 ? std::atoi(argv[2]) : 300);
    }
    if (argc > 1 && std::strcmp(argv[1], "blocks") == 0)
    {
        SetConsoleCtrlHandler(OnCtrl, TRUE);
        wc::log::InitConsole();
        return RunBlocksTest(argc > 2 ? std::atoi(argv[2]) : 240);
    }
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 120;
    const float groundZ = argc > 2 ? static_cast<float>(std::atof(argv[2])) : 64.0f;
    const float speed = argc > 3 ? static_cast<float>(std::atof(argv[3])) : 4.0f;
    SetConsoleCtrlHandler(OnCtrl, TRUE);
    wc::log::InitConsole();
    WLOG("fake_host: %d s, ground z=%.1f, walking %.1f m/s on a 24 m circle (W3 coords)", seconds, groundZ, speed);

    if (!wc::host::Start())
    {
        WLOG("fake_host: host failed to start");
        return 1;
    }
    wc::control::SetMcMode(false); // mirror mode test
    wc::host::SetWorld("fake\\flatworld.w2w");

    const float cx = 10.0f, cy = -5.0f, radius = 24.0f;
    const double t0 = GetTickCount64() / 1000.0;
    int scanIndex = 0;
    int lastPrint = -1;
    double maxGap = 0, sumGap = 0;
    int gapSamples = 0;
    while (!g_quit)
    {
        const double t = GetTickCount64() / 1000.0 - t0;
        if (t > seconds)
            break;
        // Stand still for the first 15 s (MC opens its world and waits for collision), then walk.
        const double walkT = t < 15.0 ? 0.0 : t - 15.0;
        const double ang = walkT * speed / radius;
        const float px = cx + radius * static_cast<float>(std::cos(ang));
        const float py = cy + radius * static_cast<float>(std::sin(ang));
        const float pz = Ground(static_cast<int>(std::floor(px)), 0, groundZ) +
                         (px > 20 ? (px - std::floor(px)) * 0.1f : 0.0f);
        // W3 heading (deg, CCW from +Y): direction of travel along the circle.
        const float vx = -std::sin(static_cast<float>(ang)), vy = std::cos(static_cast<float>(ang));
        const float heading = std::atan2(-vx, vy) * 57.2957795f;

        // Height samples: 33x33 around the player, 160 per tick in ring order (like the script).
        const int bx = static_cast<int>(std::floor(px)), by = static_cast<int>(std::floor(py));
        std::string batch;
        for (int n = 0; n < 160; ++n)
        {
            const int idx = scanIndex++ % (33 * 33);
            const int ox = idx % 33 - 16, oy = idx / 33 - 16;
            const int wx = bx + ox, wy = by + oy;
            char item[64];
            std::snprintf(item, sizeof(item), "%d,%d,%.3f;", wx, wy, Ground(wx, wy, groundZ));
            batch += item;
        }
        wc::host::PushHeights(batch);
        wc::host::Tick(px, py, pz, heading, -10.0f, wc::host::kTickInGame);

        const auto st = wc::host::GetStatus();
        if (st.mcAlive && st.haveMcState && t > 20.0 && st.teleportAck == st.teleportSeq)
        {
            const double gap = std::hypot(st.mcX - px, -st.mcZ - py);
            maxGap = gap > maxGap ? gap : maxGap;
            sumGap += gap;
            ++gapSamples;
        }
        if (static_cast<int>(t) != lastPrint)
        {
            lastPrint = static_cast<int>(t);
            WLOG("t=%3ds W3 player (%.2f, %.2f, %.2f) hdg %.0f | MC->W3 (%.2f, %.2f, %.2f) yaw %.0f flags 0x%X ack %s | %s", lastPrint, px, py,
                 pz, heading, st.mcX, -st.mcZ, st.mcY, st.mcYaw, st.mcFlags, st.teleportAck == st.teleportSeq ? "ok" : "holding",
                 wc::host::StatusLine().c_str());
        }
        Sleep(33);
    }
    if (gapSamples)
        WLOG("fake_host: horizontal gap MC vs W3 player while following: avg %.3f m, max %.3f m over %d samples", sumGap / gapSamples,
             maxGap, gapSamples);
    else
        WLOG("fake_host: never got a following McState (MC not linked / not in world?)");
    WLOG("fake_host: done; %s", wc::host::StatusLine().c_str());
    wc::host::Stop();
    return 0;
}
