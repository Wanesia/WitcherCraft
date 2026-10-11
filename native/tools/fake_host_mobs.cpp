// ---- WitcherCraft mobs offline test: Minecraft mobs fight the Witcher's enemies ----------------
//
//   fake_host.exe mobs [timeoutSeconds=240]
//
// Flat fake ground (script heights only, so the ground is the Witcher's, not Minecraft blocks),
// Minecraft mode, Steve in creative (hostile mobs leave him alone). Two actors are published: a
// hostile bandit 9 m east of Steve and a friendly NPC 6 m west. Then:
//   A. GOLEM: an iron golem summoned 2 m west of Steve walks over the Witcher's ground to the bandit
//      and hits it: kEvHitActor on the bandit flagged kHitByMob.
//   B. HUSK: (doesn't burn in daylight) summoned 3 m east of Steve does the same.
// Throughout: no hit on the friendly NPC, and no bandit hit without kHitByMob (Steve hits nothing).
// Exit code 0 = PASS.
#include "buildmode.hpp"
#include "collision.hpp"
#include "combat.hpp"
#include "control.hpp"
#include "log.hpp"
#include "skycraft_host.hpp"
#include "skycraft_protocol.h"

#include <Windows.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace
{
namespace proto = skycraft::proto;

volatile bool g_mobsQuit = false;
BOOL WINAPI OnCtrlMobs(DWORD)
{
    g_mobsQuit = true;
    return TRUE;
}
} // namespace

int RunMobsTest(int timeout)
{
    using namespace wc;
    SetConsoleCtrlHandler(OnCtrlMobs, TRUE);
    const float G = 64.0f;
    const uint64_t seed = (GetTickCount64() / 1000) % 200;
    const float sx = -36000.0f - static_cast<float>(seed) * 300.0f + 0.5f, sy = 1200.5f;
    WLOG("mobs test: start W3 (%.1f, %.1f, %.1f); timeout %d s", sx, sy, G, timeout);
    control::SetMcMode(true);
    if (!host::Start())
        return 1;
    host::SetWorld("fake\\mobs_test.w2w");
    collision::SetSourceActive(false);

    enum Phase
    {
        WaitDriven,
        Setup,
        Golem,
        Between,
        Husk,
        Done,
        Failed
    };
    const char* names[] = {"WaitDriven", "Setup", "Golem", "Between", "Husk", "Done", "Failed"};
    Phase phase = WaitDriven;
    double phaseT = 0;
    std::string fails;
    auto fail = [&](const char* what) {
        WLOG("mobs test: FAIL: %s", what);
        fails += what;
        fails += "; ";
    };
    float geraltX = sx, geraltY = sy, geraltZ = G;
    int scanIndex = 0;
    const int32_t banditId = -3456789, friendId = -4567890;
    float mx = 0, my = 0, mz = 0; // Steve's MC position when the actors were placed
    bool golemOk = false, huskOk = false;
    int friendHits = 0, playerFlaggedHits = 0, mobHits = 0;
    const double t0 = GetTickCount64() / 1000.0;
    double lastStep = -1, lastPrint = -1;
    auto summon = [&](const char* type, float dx) {
        char cmd[192];
        std::snprintf(cmd, sizeof(cmd), "summon minecraft:%s %.2f %.2f %.2f {Tags:[\"wc_mobtest\"],PersistenceRequired:1b}", type, mx + dx, my + 0.2f, mz);
        buildmode::QueueCommand(cmd);
        WLOG("mobs test: /%s", cmd);
    };

    while (!g_mobsQuit && phase != Done && phase != Failed)
    {
        const double t = GetTickCount64() / 1000.0 - t0;
        if (t > timeout)
        {
            WLOG("mobs test: TIMEOUT in phase %s", names[phase]);
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
            const int bx = static_cast<int>(std::floor(cx)), by = static_cast<int>(std::floor(cy));
            for (int n = 0; n < 96; ++n)
            {
                const int idx = scanIndex++ % (33 * 33);
                char item[48];
                std::snprintf(item, sizeof(item), "%d,%d,%.2f;", bx + idx % 33 - 16, by + idx / 33 - 16, G);
                batch += item;
            }
            host::PushHeights(batch);
            host::Tick(geraltX, geraltY, geraltZ, 180.0f - st.mcYaw, -st.mcPitch, host::kTickInGame);
        }

        combat::Event ev;
        while (combat::Pop(ev))
        {
            if (ev.type != proto::kEvHitActor || phase < Golem)
                continue;
            if (ev.id == friendId)
            {
                ++friendHits;
                WLOG("mobs test: the FRIENDLY NPC was hit for %.2f (flags 0x%X) -> BAD", ev.y, ev.flags);
            }
            else if (ev.id == banditId && !(ev.flags & proto::kHitByMob))
            {
                ++playerFlaggedHits;
                WLOG("mobs test: bandit hit for %.2f WITHOUT the mob flag (flags 0x%X) -> BAD", ev.y, ev.flags);
            }
            else if (ev.id == banditId)
            {
                ++mobHits;
                WLOG("mobs test: bandit hit for %.2f by a mob (flags 0x%X, knockback %.2f) in phase %s", ev.y, ev.flags, ev.z, names[phase]);
                if (phase == Golem && !golemOk)
                {
                    golemOk = true;
                    WLOG("mobs test: GOLEM: the iron golem walked over and hit the bandit after %.1f s (OK)", t - phaseT);
                }
                else if (phase == Husk && !huskOk)
                {
                    huskOk = true;
                    WLOG("mobs test: HUSK: the husk walked over and hit the bandit after %.1f s (OK)", t - phaseT);
                }
            }
        }

        switch (phase)
        {
        case WaitDriven:
            if (st.driven && inWorld && st.teleportAck == st.teleportSeq && !st.gateHolding && fr.eyeValid && t > 3.0)
            {
                for (const char* c : {"gamemode creative", "kill @e[tag=wc_mobtest]"})
                    buildmode::QueueCommand(c);
                mx = static_cast<float>(st.mcX);
                my = static_cast<float>(st.mcY);
                mz = static_cast<float>(st.mcZ);
                // id,x,y,z,heading,w,h,hp,flags,level,name; in W3 coords (MC x, -MC z, MC y).
                char rec[320];
                std::snprintf(rec, sizeof(rec), "%d,%.2f,%.2f,%.2f,90.0,0.60,1.80,1.000,1,10,Test Bandit;%d,%.2f,%.2f,%.2f,270.0,0.60,1.80,1.000,0,10,Test Friend;",
                              banditId, mx + 9.0f, -mz, my, friendId, mx - 6.0f, -mz, my);
                combat::SetActors(rec);
                WLOG("mobs test: Minecraft drives at MC (%.2f, %.2f, %.2f); hostile bandit 9 m east, friendly NPC 6 m west", mx, my, mz);
                phase = Setup;
                phaseT = t;
            }
            break;
        case Setup:
            if (t - phaseT > 3.0 && buildmode::CommandsPending() == 0)
            {
                summon("iron_golem", -2.0f);
                phase = Golem;
                phaseT = t;
            }
            break;
        case Golem:
            if (golemOk || t - phaseT > 30.0)
            {
                if (!golemOk)
                    fail("golem: no mob hit on the bandit in 30 s");
                buildmode::QueueCommand("kill @e[tag=wc_mobtest]");
                phase = Between;
                phaseT = t;
            }
            break;
        case Between:
            if (t - phaseT > 3.0 && buildmode::CommandsPending() == 0)
            {
                summon("husk", 3.0f);
                phase = Husk;
                phaseT = t;
            }
            break;
        case Husk:
            if ((huskOk && t - phaseT > 4.0) || t - phaseT > 30.0)
            {
                if (!huskOk)
                    fail("husk: no mob hit on the bandit in 30 s");
                phase = Done;
            }
            break;
        default:
            break;
        }
        if (t - lastPrint >= 2.0)
        {
            lastPrint = t;
            WLOG("t=%5.1fs %s | %s", t, names[phase], host::StatusLine().c_str());
        }
        Sleep(8);
    }
    combat::SetActors("");
    buildmode::QueueCommand("kill @e[tag=wc_mobtest]");
    buildmode::QueueCommand("gamemode survival");
    for (int i = 0; i < 90; ++i)
    {
        control::OnFrame(0, 0.0f, 0.0f);
        host::Tick(geraltX, geraltY, geraltZ, 0.0f, 0.0f, host::kTickInGame);
        Sleep(33);
    }
    if (friendHits > 0)
        fails += "friendly NPC hit; ";
    if (playerFlaggedHits > 0)
        fails += "mob hit without the mob flag; ";
    const bool pass = phase == Done && fails.empty() && golemOk && huskOk;
    WLOG("mobs test: %s (golem %d husk %d | %d mob hit(s) on the bandit, %d unflagged, %d on the friendly NPC) %s", pass ? "PASS" : "FAIL", golemOk, huskOk,
         mobHits, playerFlaggedHits, friendHits, fails.c_str());
    host::Stop();
    return pass ? 0 : 2;
}
