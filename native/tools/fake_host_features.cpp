// ---- WitcherCraft features offline test: Roach, torch light, boss bar --------------------------
//
//   fake_host.exe features [timeoutSeconds=180]
//
// Flat fake ground (script heights only, no PhysX source), Minecraft mode, then:
//   A. ROACH: "Call Roach" (X) twice: Minecraft's log must say "Roach created" (or "called"),
//      then "Roach called" (the same horse comes back).
//   B. LIGHT: /setblock a torch 2 blocks away: the WcLights block (protocol @0xA00) must list a
//      level-14 warm light at that block.
//   C. BOSS: publish a hostile, fighting boss actor 5 m away: Minecraft's log must say
//      "boss bar for Test Boss".
//   D. FIRE: a bandit 2.5 m ahead of Steve. Lava at its feet -> kEvActorFire (seconds > 0); lava
//      gone -> Minecraft's after-burn is NOT forwarded as hits (the Witcher's burning does that);
//      the actor table says "burning" + water at its feet -> kEvActorFire 0 (put out); flint and
//      steel used on it (right click) -> kEvActorFire again. The lava's hits come flagged kHitFire
//      (the Witcher turns those into fire damage). Then the table says "no fire" (64: the Witcher
//      refused burning / a crowd NPC) + lava again -> NO kEvActorFire (Minecraft keeps its fire off).
//   E. KEYS: McState wcKeys reached the host: interact G, switch F8, senses V, Roach X.
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

namespace
{
namespace proto = skycraft::proto;

volatile bool g_featQuit = false;
BOOL WINAPI OnCtrlFeat(DWORD)
{
    g_featQuit = true;
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

// Does Minecraft's log contain `what` after byte `from`?
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
} // namespace

int RunFeaturesTest(int timeout)
{
    using namespace wc;
    SetConsoleCtrlHandler(OnCtrlFeat, TRUE);
    const float G = 64.0f;
    const uint64_t seed = (GetTickCount64() / 1000) % 200;
    const float sx = -30000.0f - static_cast<float>(seed) * 300.0f + 0.5f, sy = 900.5f;
    WLOG("features test: start W3 (%.1f, %.1f, %.1f); timeout %d s", sx, sy, G, timeout);
    control::SetMcMode(true);
    if (!host::Start())
        return 1;
    host::SetWorld("fake\\features_test.w2w");
    collision::SetSourceActive(false);

    enum Phase
    {
        WaitDriven,
        Roach1,
        Roach2,
        Light,
        Boss,
        FireSetup,
        FireLava,
        FireAfterburn,
        FireDouse,
        FireFlintWait,
        FireFlint,
        FireNoFire,
        Done,
        Failed
    };
    const char* names[] = {"WaitDriven", "Roach1", "Roach2", "Light", "Boss", "FireSetup", "FireLava", "FireAfterburn", "FireDouse", "FireFlintWait",
                           "FireFlint", "FireNoFire", "Done", "Failed"};
    Phase phase = WaitDriven;
    double phaseT = 0;
    std::string fails;
    auto fail = [&](const char* what) {
        WLOG("features test: FAIL: %s", what);
        fails += what;
        fails += "; ";
    };
    float geraltX = sx, geraltY = sy, geraltZ = G;
    int scanIndex = 0;
    size_t log0 = 0;
    bool roachOk = false, roach2Ok = false, lightOk = false, bossOk = false;
    const int32_t bossId = -7654321;
    const double t0 = GetTickCount64() / 1000.0;
    double lastStep = -1, lastPrint = -1;
    int lightBx = 0, lightBy = 0, lightBz = 0;
    // D. fire
    const int32_t fireId = -2345678;
    float fx = 0, fy = 0, fz = 0, fHeading = 0; // the bandit, W3 coords
    int fbx = 0, fby = 0, fbz = 0;              // the MC block at its feet
    bool igniteOk = false, douseOk = false, flintOk = false, afterburnOk = false, lavaFlagOk = false, noFireOk = false, noFireLava = false;
    int afterburnHits = 0, fireEvents = 0, noFireIgnites = 0, noFireHits = 0;
    auto publishBandit = [&](bool burning, bool noFire = false) {
        char rec[160];
        std::snprintf(rec, sizeof(rec), "%d,%.2f,%.2f,%.2f,%.1f,0.60,1.80,1.000,%d,10,Fire Bandit;", fireId, fx, fy, fz, fHeading,
                      1 + (burning ? 32 : 0) + (noFire ? 64 : 0));
        combat::SetActors(rec);
    };
    auto fill = [&](const char* block, int radius) {
        char cmd[128];
        std::snprintf(cmd, sizeof(cmd), "fill %d %d %d %d %d %d %s", fbx - radius, fby, fbz - radius, fbx + radius, fby + 1, fbz + radius, block);
        buildmode::QueueCommand(cmd);
        WLOG("features test: FIRE: /%s", cmd);
    };
    auto setblock = [&](const char* block) {
        char cmd[128];
        std::snprintf(cmd, sizeof(cmd), "setblock %d %d %d %s", fbx, fby, fbz, block);
        buildmode::QueueCommand(cmd);
        WLOG("features test: FIRE: /%s", cmd);
    };

    while (!g_featQuit && phase != Done && phase != Failed)
    {
        const double t = GetTickCount64() / 1000.0 - t0;
        if (t > timeout)
        {
            WLOG("features test: TIMEOUT in phase %s", names[phase]);
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

        // D. the fire phases read the combat events (kEvActorFire, kEvHitActor) on the bandit.
        combat::Event ev;
        while (combat::Pop(ev))
        {
            if (ev.id != fireId || phase < FireLava)
                continue;
            if (ev.type == proto::kEvActorFire)
            {
                ++fireEvents;
                WLOG("features test: FIRE: kEvActorFire on the bandit, %.1f s (phase %s)", ev.x, names[phase]);
                if (phase == FireLava && ev.x > 0.0f && !igniteOk)
                {
                    igniteOk = true;
                    WLOG("features test: FIRE: lava set the bandit burning (OK)");
                }
                else if (phase == FireDouse && ev.x == 0.0f && !douseOk)
                {
                    douseOk = true;
                    WLOG("features test: FIRE: water put the bandit out (OK)");
                }
                else if (phase == FireFlint && ev.x > 0.0f && !flintOk)
                {
                    flintOk = true;
                    WLOG("features test: FIRE: flint and steel set the bandit burning (OK)");
                }
                else if (phase == FireNoFire && ev.x > 0.0f && t - phaseT > 0.5)
                {
                    ++noFireIgnites;
                    WLOG("features test: FIRE: kEvActorFire on a no-fire actor -> BAD");
                }
            }
            else if (ev.type == proto::kEvHitActor && phase == FireLava && !lavaFlagOk)
            {
                lavaFlagOk = (ev.flags & proto::kHitFire) != 0;
                WLOG("features test: FIRE: lava hit %.2f, flags 0x%X -> %s", ev.y, ev.flags, lavaFlagOk ? "fire-flagged (OK)" : "NOT fire-flagged (BAD)");
            }
            else if (ev.type == proto::kEvHitActor && phase == FireNoFire && noFireLava)
            {
                ++noFireHits;
            }
            else if (ev.type == proto::kEvHitActor && phase == FireAfterburn && t - phaseT > 1.5)
            {
                ++afterburnHits;
                WLOG("features test: FIRE: after-burn hit %.2f forwarded (flags 0x%X) -> BAD", ev.y, ev.flags);
            }
        }

        switch (phase)
        {
        case WaitDriven:
            if (st.driven && inWorld && st.teleportAck == st.teleportSeq && !st.gateHolding && fr.eyeValid && t > 3.0)
            {
                log0 = McLogSize();
                WLOG("features test: Minecraft drives at W3 (%.2f, %.2f, %.2f); ROACH: X", st.mcX, -st.mcZ, st.mcY);
                buildmode::QueueEvent({proto::kInKey, 27, 1, 0, 0}); // SDL scancode X
                buildmode::QueueEvent({proto::kInKey, 27, 0, 0, 0});
                phase = Roach1;
                phaseT = t;
            }
            break;
        case Roach1:
            if (McLogHas("WitcherCraft: Roach created", log0) || McLogHas("WitcherCraft: Roach called", log0))
            {
                roachOk = true;
                WLOG("features test: ROACH: first X -> Roach is there (OK)");
                log0 = McLogSize();
                buildmode::QueueEvent({proto::kInKey, 27, 1, 0, 0});
                buildmode::QueueEvent({proto::kInKey, 27, 0, 0, 0});
                phase = Roach2;
                phaseT = t;
            }
            else if (t - phaseT > 6.0)
            {
                fail("roach: X did nothing");
                phase = Roach2;
                phaseT = t;
            }
            break;
        case Roach2:
            if (!roach2Ok && McLogHas("WitcherCraft: Roach called", log0))
            {
                roach2Ok = true;
                WLOG("features test: ROACH: second X -> the same Roach called back (OK)");
            }
            if (roach2Ok || t - phaseT > 6.0)
            {
                if (!roach2Ok)
                    fail("roach: second X");
                lightBx = static_cast<int>(std::floor(st.mcX)) + 2;
                lightBy = static_cast<int>(std::floor(st.mcY));
                lightBz = static_cast<int>(std::floor(st.mcZ));
                char cmd[96];
                std::snprintf(cmd, sizeof(cmd), "setblock %d %d %d minecraft:torch", lightBx, lightBy, lightBz);
                buildmode::QueueCommand(cmd);
                WLOG("features test: LIGHT: /%s", cmd);
                phase = Light;
                phaseT = t;
            }
            break;
        case Light:
            if (t - phaseT > 1.0)
            {
                const auto* L = reinterpret_cast<const proto::WcLights*>(host::MappingBase() + proto::kOffWcLights);
                const uint32_t n = std::min(L->count, proto::kMaxWcLights);
                for (uint32_t i = 0; i < n && !lightOk; ++i)
                    lightOk = L->lights[i].x == lightBx && L->lights[i].y == lightBy && L->lights[i].z == lightBz && L->lights[i].level == 14 &&
                              L->lights[i].kind == proto::kWcLightWarm;
                if (lightOk || t - phaseT > 8.0)
                {
                    WLOG("features test: LIGHT: %u light(s) listed (version %u) -> %s", L->count, L->version,
                         lightOk ? "THE TORCH IS LISTED (OK)" : "BAD");
                    if (!lightOk)
                        fail("light: torch not listed");
                    // C: a boss 5 m east of Steve: hostile (1) + in combat (8) + boss (16).
                    char rec[160];
                    std::snprintf(rec, sizeof(rec), "%d,%.2f,%.2f,%.2f,0.0,0.80,2.40,0.750,25,30,Test Boss;", bossId, st.mcX + 5.0, -st.mcZ, st.mcY);
                    combat::SetActors(rec);
                    log0 = McLogSize();
                    phase = Boss;
                    phaseT = t;
                }
            }
            break;
        case Boss:
            if (McLogHas("boss bar for Test Boss", log0) || t - phaseT > 6.0)
            {
                bossOk = McLogHas("boss bar for Test Boss", log0);
                WLOG("features test: BOSS: %s", bossOk ? "Minecraft boss bar shown (OK)" : "no boss bar (BAD)");
                if (!bossOk)
                    fail("boss: no boss bar");
                // D: a bandit 2.5 m ahead of a level-looking Steve, facing him (as the combat test).
                for (const char* c : {"gamemode creative", "item replace entity @s weapon.mainhand with minecraft:flint_and_steel"})
                    buildmode::QueueCommand(c);
                float yaw = 0, pitch = 0;
                control::GetLook(yaw, pitch);
                control::SetLook(yaw, 0.0f);
                const double r = yaw * 3.14159265358979 / 180.0;
                fx = static_cast<float>(st.mcX - std::sin(r) * 2.5);
                fy = static_cast<float>(-(st.mcZ + std::cos(r) * 2.5));
                fz = static_cast<float>(st.mcY);
                fHeading = -yaw;
                fbx = static_cast<int>(std::floor(fx));
                fby = static_cast<int>(std::floor(fz));
                fbz = static_cast<int>(std::floor(-fy));
                publishBandit(false);
                WLOG("features test: FIRE: bandit %d at W3 (%.2f, %.2f, %.2f), block %d %d %d", fireId, fx, fy, fz, fbx, fby, fbz);
                phase = FireSetup;
                phaseT = t;
            }
            break;
        case FireSetup:
            if (t - phaseT > 2.5 && buildmode::CommandsPending() == 0)
            {
                setblock("minecraft:lava");
                phase = FireLava;
                phaseT = t;
            }
            break;
        case FireLava:
            if (igniteOk || t - phaseT > 6.0)
            {
                if (!igniteOk)
                    fail("fire: lava did not set the bandit burning");
                fill("minecraft:air", 2);
                phase = FireAfterburn;
                phaseT = t;
            }
            break;
        case FireAfterburn:
            // Minecraft keeps the stand-in burning for ~15 s; its 1/s burn must not come through.
            if (t - phaseT > 4.5)
            {
                afterburnOk = afterburnHits == 0;
                WLOG("features test: FIRE: %d after-burn hit(s) forwarded in 3 s -> %s", afterburnHits, afterburnOk ? "OK" : "BAD");
                if (!afterburnOk)
                    fail("fire: Minecraft after-burn forwarded");
                publishBandit(true); // the Witcher: it burns now
                setblock("minecraft:water");
                phase = FireDouse;
                phaseT = t;
            }
            break;
        case FireDouse:
            if (douseOk || t - phaseT > 5.0)
            {
                if (!douseOk)
                    fail("fire: water did not put the bandit out");
                fill("minecraft:air", 2);
                publishBandit(false);
                phase = FireFlintWait;
                phaseT = t;
            }
            break;
        case FireFlintWait:
            if (t - phaseT > 2.0 && buildmode::CommandsPending() == 0)
            {
                WLOG("features test: FIRE: right click the bandit with flint and steel");
                buildmode::QueueEvent({proto::kInMouseButton, 3, 1, 0, 0});
                phase = FireFlint;
                phaseT = t;
            }
            break;
        case FireFlint:
            if (t - phaseT > 0.15 && t - phaseT < 0.25)
                buildmode::QueueEvent({proto::kInMouseButton, 3, 0, 0, 0});
            if (flintOk || t - phaseT > 4.0)
            {
                if (!flintOk)
                    fail("fire: flint and steel did not set the bandit burning");
                // Minecraft's flint fire is still on the stand-in; now the Witcher says "can't burn".
                publishBandit(false, true);
                WLOG("features test: FIRE: the actor table says the bandit can't burn (flag 64)");
                phase = FireNoFire;
                phaseT = t;
            }
            break;
        case FireNoFire:
            if (!noFireLava && t - phaseT > 1.0)
            {
                setblock("minecraft:lava");
                noFireLava = true;
            }
            if (t - phaseT > 5.0)
            {
                noFireOk = noFireIgnites == 0;
                WLOG("features test: FIRE: no-fire actor: %d ignite event(s) in 4.5 s (lava and the flint fire), %d lava hit(s) still sent -> %s",
                     noFireIgnites, noFireHits, noFireOk ? "OK" : "BAD");
                if (!noFireOk)
                    fail("fire: a no-fire actor was set burning");
                fill("minecraft:air", 2);
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
    buildmode::QueueCommand("kill @e[type=minecraft:horse,tag=witchercraft_roach]");
    if (phase >= FireSetup)
        fill("minecraft:air", 2);
    buildmode::QueueCommand("gamemode survival");
    for (int i = 0; i < 90; ++i)
    {
        control::OnFrame(0, 0.0f, 0.0f);
        host::Tick(geraltX, geraltY, geraltZ, 0.0f, 0.0f, host::kTickInGame);
        Sleep(33);
    }
    // E. the keys Minecraft's WitcherCraft controls sent (McState wcKeys -> control).
    const bool keysOk = control::KeyDik(proto::kWcKeyInteract) == 0x22 && control::KeyDik(proto::kWcKeyMode) == 0x42 &&
                        control::KeyDik(proto::kWcKeySenses) == 0x2F && control::KeyDik(proto::kWcKeyRoach) == 0x2D &&
                        (control::WcFlags() & proto::kWcPanelKeys);
    WLOG("features test: KEYS: interact %s, switch %s, senses %s, Roach %s, panel keys %s -> %s", control::KeyName(control::KeyDik(0)).c_str(),
         control::KeyName(control::KeyDik(1)).c_str(), control::KeyName(control::KeyDik(2)).c_str(), control::KeyName(control::KeyDik(3)).c_str(),
         (control::WcFlags() & proto::kWcPanelKeys) ? "on" : "off", keysOk ? "OK" : "BAD (or the options were changed from the defaults)");
    if (!keysOk)
        fails += "keys; ";
    if (!lavaFlagOk)
        fails += "fire: lava hits not fire-flagged; ";
    const bool pass = phase == Done && fails.empty() && roachOk && roach2Ok && lightOk && bossOk && igniteOk && afterburnOk && douseOk && flintOk && lavaFlagOk &&
                      noFireOk && keysOk;
    WLOG("features test: %s (roach %d again %d light %d boss %d | fire: lava %d [fire flag %d] after-burn %d water %d flint %d no-fire %d, %d events | keys %d) %s",
         pass ? "PASS" : "FAIL", roachOk, roach2Ok, lightOk, bossOk, igniteOk, lavaFlagOk, afterburnOk, douseOk, flintOk, noFireOk, fireEvents, keysOk,
         fails.c_str());
    host::Stop();
    return pass ? 0 : 2;
}

// ---- fake_host.exe idle [seconds=90]: keep Minecraft linked on flat fake ground, nothing else --
// (for looking at Minecraft-side things offline, e.g. its WITCHERCRAFT_SCREEN_TOUR screenshots).
int RunIdle(int seconds)
{
    using namespace wc;
    SetConsoleCtrlHandler(OnCtrlFeat, TRUE);
    const float G = 64.0f, sx = -40000.5f, sy = 900.5f;
    control::SetMcMode(true);
    if (!host::Start())
        return 1;
    host::SetWorld("fake/idle.w2w");
    collision::SetSourceActive(false);
    float gx = sx, gy = sy, gz = G;
    int scanIndex = 0;
    const double t0 = GetTickCount64() / 1000.0;
    double lastPrint = -1;
    while (!g_featQuit && GetTickCount64() / 1000.0 - t0 < seconds)
    {
        const double t = GetTickCount64() / 1000.0 - t0;
        const control::Frame fr = control::OnFrame(0, 0.0f, 0.0f);
        const auto st = host::GetStatus();
        if (fr.engaged && fr.eyeValid && fr.arrived)
        {
            gx = static_cast<float>(fr.feet[0]);
            gy = static_cast<float>(fr.feet[1]);
            gz = static_cast<float>(fr.feet[2]);
        }
        float cx = gx, cy = gy, cz = gz;
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
        host::Tick(gx, gy, gz, 180.0f - st.mcYaw, -st.mcPitch, host::kTickInGame);
        if (t - lastPrint >= 5.0)
        {
            lastPrint = t;
            WLOG("idle t=%5.1fs | %s", t, host::StatusLine().c_str());
        }
        Sleep(33);
    }
    host::Stop();
    return 0;
}
