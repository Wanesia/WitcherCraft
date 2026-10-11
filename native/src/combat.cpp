// WitcherCraft M4a combat (see combat.hpp and). Ported from SkyCraft's
// skse/src/Combat.cpp (WriteActorTable, SendHurt / BridgePlayerDamage, the PerFrame event switch).
#include "combat.hpp"

#include "buildmode.hpp"
#include "control.hpp"
#include "log.hpp"
#include "skycraft_protocol.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace wc::combat
{
namespace proto = skycraft::proto;

namespace
{
constexpr uint64_t kDotFlushMs = 500;  // SkyCraft kDotFlushSeconds: magic / other damage is batched
constexpr size_t kMaxQueued = 256;     // events waiting for the script
constexpr float kMcPlayerHealth = 20.0f;

template <class T>
std::atomic_ref<T> A(T& v)
{
    return std::atomic_ref<T>(v);
}

std::once_flag g_cfgOnce;
Config g_cfg;

float IniFloat(const wchar_t* path, const wchar_t* key, float def)
{
    wchar_t buf[64]{};
    GetPrivateProfileStringW(L"WitcherCraft", key, L"", buf, 64, path);
    if (!buf[0])
        return def;
    wchar_t* end = nullptr;
    const double v = std::wcstod(buf, &end);
    return end != buf && std::isfinite(v) ? static_cast<float>(v) : def;
}

void LoadConfig()
{
    // witchercraft_native.ini next to this module (the DLL in game, the exe in offline tools).
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&LoadConfig), &self);
    wchar_t path[MAX_PATH * 2]{};
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH * 2);
    wchar_t* slash = (n && n < MAX_PATH * 2) ? wcsrchr(path, L'\\') : nullptr;
    if (slash)
    {
        wcscpy_s(slash + 1, MAX_PATH * 2 - (slash + 1 - path), L"witchercraft_native.ini");
        g_cfg.enabled = GetPrivateProfileIntW(L"WitcherCraft", L"Combat", 1, path) != 0;
        g_cfg.range = std::clamp(IniFloat(path, L"CombatRange", g_cfg.range), 4.0f, 128.0f);
        g_cfg.hitScale = std::clamp(IniFloat(path, L"CombatHitScale", g_cfg.hitScale), 0.0f, 10.0f);
        g_cfg.hitType = static_cast<int>(GetPrivateProfileIntW(L"WitcherCraft", L"CombatHitType", 0, path)) != 0 ? 1 : 0;
        g_cfg.hurtScale = std::clamp(IniFloat(path, L"CombatHurtScale", g_cfg.hurtScale), 0.0f, 100.0f);
        g_cfg.deathMode = static_cast<int>(GetPrivateProfileIntW(L"WitcherCraft", L"CombatDeathMode", 0, path)) != 0 ? 1 : 0;
    }
    WLOG("combat: config Combat=%d CombatRange=%.0f CombatHitScale=%.3f CombatHitType=%d CombatHurtScale=%.2f CombatDeathMode=%d", g_cfg.enabled,
         g_cfg.range, g_cfg.hitScale, g_cfg.hitType, g_cfg.hurtScale, g_cfg.deathMode);
}

// ---- shared state -----------------------------------------------------------------------
std::mutex g_mutex;
std::vector<proto::ActorRecord> g_actors; // MC coords, ready to publish
bool g_actorsDirty = false;
std::deque<Event> g_events;  // MC -> script
struct Dot
{
    int32_t attacker = 0;
    int kind = proto::kHurtOther;
    float mc = 0; // Minecraft damage accumulated
    uint64_t sinceMs = 0;
};
Dot g_dot;
std::vector<proto::InputEvent> g_hurtOut; // immediate kInHurt events, sent by the host thread
Stats g_stats;
int g_hitLogs = 0, g_hurtLogs = 0, g_eventLogs = 0;

float NormDeg(float d)
{
    d = std::fmod(d + 180.0f, 360.0f);
    if (d < 0)
        d += 360.0f;
    return d - 180.0f;
}

// "a,b,c,...": next comma field.
bool Field(std::string_view& s, std::string_view& out)
{
    if (s.empty())
        return false;
    const size_t c = s.find(',');
    out = s.substr(0, c);
    s = c == std::string_view::npos ? std::string_view{} : s.substr(c + 1);
    return true;
}

bool ParseFloat(std::string_view f, float& v)
{
    char buf[48];
    if (f.empty() || f.size() >= sizeof(buf))
        return false;
    std::memcpy(buf, f.data(), f.size());
    buf[f.size()] = 0;
    char* end = nullptr;
    const double d = std::strtod(buf, &end);
    if (end == buf || !std::isfinite(d))
        return false;
    v = static_cast<float>(d);
    return true;
}

bool ParseInt(std::string_view f, long long& v)
{
    char buf[32];
    if (f.empty() || f.size() >= sizeof(buf))
        return false;
    std::memcpy(buf, f.data(), f.size());
    buf[f.size()] = 0;
    char* end = nullptr;
    v = std::strtoll(buf, &end, 10);
    return end != buf;
}

void QueueHurtLocked(int kind, float mc, int32_t attacker, uint32_t flags)
{
    if (mc <= 0.01f)
        return;
    // SkyCraft's wire format: a = "Skyrim damage" * 100, which Minecraft divides by 100 and by
    // SkyCombat.SKYRIM_TO_MC_DAMAGE (5). We send Minecraft damage * 5 so it arrives exactly.
    const float wire = mc * 5.0f * 100.0f;
    const int32_t a = static_cast<int32_t>(std::lround(std::min(wire, 2.0e9f)));
    g_hurtOut.push_back({proto::kInHurt, static_cast<uint16_t>(kind), a, attacker, static_cast<int32_t>(flags)});
    g_stats.hurtsSent++;
    g_stats.lastHurtMc = mc;
    if (g_hurtLogs < 200)
    {
        ++g_hurtLogs;
        WLOG("combat: player hit by %d: %.2f Minecraft damage (%s)%s%s", attacker, mc,
             kind == proto::kHurtMelee ? "melee" : kind == proto::kHurtProjectile ? "projectile" : kind == proto::kHurtMagic ? "magic" : "other",
             (flags & proto::kHurtPowerAttack) ? ", power attack" : "", (flags & proto::kHurtBlockedInSkyrim) ? ", blocked" : "");
    }
}
} // namespace

const Config& GetConfig()
{
    std::call_once(g_cfgOnce, LoadConfig);
    return g_cfg;
}

int SetActors(std::string_view text)
{
    const Config& cfg = GetConfig();
    std::vector<proto::ActorRecord> out;
    size_t pos = 0;
    while (cfg.enabled && pos < text.size() && out.size() < proto::kMaxActors)
    {
        size_t end = text.find(';', pos);
        if (end == std::string_view::npos)
            end = text.size();
        std::string_view rec = text.substr(pos, end - pos);
        pos = end + 1;
        std::string_view f;
        long long id = 0, flags = 0, level = 0;
        float x = 0, y = 0, z = 0, heading = 0, width = 0, height = 0, hp = 0;
        if (!Field(rec, f) || !ParseInt(f, id) || id == 0)
            continue;
        bool ok = Field(rec, f) && ParseFloat(f, x);
        ok = ok && Field(rec, f) && ParseFloat(f, y);
        ok = ok && Field(rec, f) && ParseFloat(f, z);
        ok = ok && Field(rec, f) && ParseFloat(f, heading);
        ok = ok && Field(rec, f) && ParseFloat(f, width);
        ok = ok && Field(rec, f) && ParseFloat(f, height);
        ok = ok && Field(rec, f) && ParseFloat(f, hp);
        ok = ok && Field(rec, f) && ParseInt(f, flags);
        ok = ok && Field(rec, f) && ParseInt(f, level);
        if (!ok)
            continue;
        proto::ActorRecord r{};
        r.formId = static_cast<uint32_t>(static_cast<int32_t>(id));
        r.flags = static_cast<uint32_t>(flags);
        // W3 (x east, y north, z up) -> MC (x, y up, z south); heading CCW from north -> MC yaw.
        r.x = x;
        r.y = z;
        r.z = -y;
        r.yaw = NormDeg(180.0f - heading);
        r.width = std::clamp(width > 0.0f ? width : 0.6f, 0.3f, 6.0f);
        r.height = std::clamp(height > 0.0f ? height : 1.8f, 0.3f, 12.0f);
        r.healthFrac = std::clamp(hp, 0.0f, 1.0f);
        r.level = static_cast<uint16_t>(std::clamp<long long>(level, 0, 65535));
        // The rest of the record is the name (may contain commas).
        const size_t n = std::min(rec.size(), sizeof(r.name) - 1);
        std::memcpy(r.name, rec.data(), n);
        // Don't cut a UTF-8 sequence in half: if the first byte left out continues a sequence,
        // leave out that whole sequence.
        size_t len = n;
        if (n < rec.size())
            while (len > 0 && (static_cast<unsigned char>(rec[len]) & 0xC0) == 0x80)
                --len;
        r.name[len] = 0;
        out.push_back(r);
    }
    std::lock_guard lk(g_mutex);
    const bool changed = out.size() != g_actors.size() ||
                         (!out.empty() && std::memcmp(out.data(), g_actors.data(), out.size() * sizeof(proto::ActorRecord)) != 0);
    if (changed)
    {
        g_actors = std::move(out);
        g_actorsDirty = true;
    }
    return static_cast<int>(g_actors.size());
}

float Hurt(int kind, float w3Damage, float maxHealth, int32_t attacker, uint32_t flags)
{
    const Config& cfg = GetConfig();
    if (!cfg.enabled || !(w3Damage > 0.0f))
        return 0.0f;
    if (kind < proto::kHurtMelee || kind > proto::kHurtOther)
        kind = proto::kHurtOther;
    // The same share of Steve's 20 health as W3 would have taken of Geralt's, times the knob.
    const float mc = maxHealth > 1.0f ? w3Damage / maxHealth * kMcPlayerHealth * cfg.hurtScale : w3Damage / 100.0f * cfg.hurtScale;
    std::lock_guard lk(g_mutex);
    if (kind == proto::kHurtMelee || kind == proto::kHurtProjectile)
    {
        QueueHurtLocked(kind, mc, attacker, flags);
    }
    else
    {
        if (g_dot.mc <= 0.0f)
            g_dot.sinceMs = GetTickCount64();
        g_dot.attacker = attacker ? attacker : g_dot.attacker;
        g_dot.kind = kind;
        g_dot.mc += mc;
    }
    return mc;
}

bool Pop(Event& out)
{
    std::lock_guard lk(g_mutex);
    if (g_events.empty())
        return false;
    out = g_events.front();
    g_events.pop_front();
    return true;
}

void OnMcEvent(const proto::McEvent& ev)
{
    const Config& cfg = GetConfig();
    Event e;
    e.type = ev.type;
    e.id = static_cast<int32_t>(ev.formId);
    std::lock_guard lk(g_mutex);
    switch (ev.type)
    {
    case proto::kEvHitActor: {
        g_stats.hits++;
        g_stats.lastHitId = e.id;
        g_stats.lastHitMc = ev.a;
        e.flags = ev.flags;
        e.weapon = ev.weapon;
        e.x = std::max(0.0f, ev.a) * cfg.hitScale;
        e.y = ev.a;
        e.z = ev.d;
        // MC push direction (x, z) -> W3 (x, -z) -> W3 heading (forward = (-sin h, cos h)).
        e.w = (ev.b != 0.0f || ev.c != 0.0f) ? static_cast<float>(std::atan2(-ev.b, -ev.c) * 57.29577951308232) : 0.0f;
        if (g_hitLogs < 200)
        {
            ++g_hitLogs;
            WLOG("combat: Minecraft hit actor %d for %.2f (-> %.1f %% of its health)%s%s%s%s, weapon %u, knockback %.2f", e.id, ev.a, e.x * 100.0f,
                 (ev.flags & proto::kHitCritical) ? ", critical" : "", (ev.flags & proto::kHitProjectile) ? ", projectile" : "",
                 (ev.flags & proto::kHitFire) ? ", fire" : "", (ev.flags & proto::kHitByMob) ? ", by a Minecraft mob" : "", ev.weapon, ev.d);
        }
        break;
    }
    case proto::kEvPlayerDied:
        g_stats.deaths++;
        WLOG("combat: the Minecraft player died (killer %d); death mode %d", e.id, cfg.deathMode);
        break;
    case proto::kEvExplosion:
        g_stats.explosions++;
        e.x = ev.a;
        e.y = -ev.c;
        e.z = ev.b;
        e.w = ev.d;
        WLOG("combat: Minecraft explosion at W3 (%.1f, %.1f, %.1f), radius %.1f", e.x, e.y, e.z, e.w);
        break;
    case proto::kEvActorFire:
        // A stand-in caught fire (lava, fire, fire aspect, flame arrows) or water put it out.
        g_stats.fires++;
        e.x = std::max(0.0f, ev.a);
        WLOG("combat: Minecraft %s actor %d%s", e.x > 0.0f ? "set fire to" : "put out the fire on", e.id,
             e.x > 0.0f ? (" for " + std::to_string(static_cast<int>(e.x + 0.5f)) + " s").c_str() : "");
        break;
    case proto::kEvLeaveMcMode:
        // Minecraft's pause menu "Quit to The Witcher": the same as F8 (control::OnFrame disengages).
        WLOG("control: Minecraft's pause menu asked to leave Minecraft mode");
        control::SetMcMode(false);
        return;
    case proto::kEvArrowStuck:
        g_stats.arrows++; // SkyCraft only draws the stuck arrow; the hit itself is a kEvHitActor
        return;
    default:
        g_stats.otherEvents++;
        if (g_eventLogs < 20)
        {
            ++g_eventLogs;
            WLOG("combat: MC event type=%u id=%d a=%.2f (not used)", ev.type, e.id, ev.a);
        }
        return;
    }
    if (!cfg.enabled)
        return;
    if (g_events.size() >= kMaxQueued)
        g_events.pop_front();
    g_events.push_back(e);
}

void HostPump(uint8_t* base, bool mcAlive)
{
    if (!base)
        return;
    std::vector<proto::InputEvent> hurts;
    {
        std::lock_guard lk(g_mutex);
        if (g_actorsDirty)
        {
            g_actorsDirty = false;
            auto* table = reinterpret_cast<proto::ActorTable*>(base + proto::kOffActorTable);
            auto seq = A(table->seq);
            const uint32_t s = seq.load(std::memory_order_relaxed);
            seq.store(s + 1, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            const uint32_t count = static_cast<uint32_t>(std::min<size_t>(g_actors.size(), proto::kMaxActors));
            table->count = count;
            if (count)
                std::memcpy(table->actors, g_actors.data(), sizeof(proto::ActorRecord) * count);
            seq.store(s + 2, std::memory_order_release);
            g_stats.tablesWritten++;
            g_stats.actorsPublished = count;
        }
        if (g_dot.mc > 0.0f && GetTickCount64() - g_dot.sinceMs >= kDotFlushMs)
        {
            QueueHurtLocked(g_dot.kind, g_dot.mc, g_dot.attacker, 0);
            g_dot = Dot{};
        }
        hurts.swap(g_hurtOut);
    }
    if (!mcAlive)
        return; // stale damage must not land when Minecraft comes back
    for (const auto& ev : hurts)
        buildmode::QueueEvent(ev); // the host thread's input-ring writer sends it this iteration
}

Stats GetStats()
{
    std::lock_guard lk(g_mutex);
    return g_stats;
}

std::string StatusSuffix()
{
    const Stats s = GetStats();
    char buf[160];
    std::snprintf(buf, sizeof(buf), " | combat actors=%llu hits=%llu hurts=%llu deaths=%llu fires=%llu", static_cast<unsigned long long>(s.actorsPublished),
                  static_cast<unsigned long long>(s.hits), static_cast<unsigned long long>(s.hurtsSent), static_cast<unsigned long long>(s.deaths),
                  static_cast<unsigned long long>(s.fires));
    return buf;
}
} // namespace wc::combat
