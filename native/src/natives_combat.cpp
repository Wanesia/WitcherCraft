// WitcherCraft M4a combat natives. See combat.hpp.
//
// WitcherScript side (witchercraft_combat.ws):
//   import function WCraft_CombatActors( data : string ) : int;   // "id,x,y,z,heading,w,h,hp,flags,level,name;..." W3 coords, "" = none
//   import function WCraft_CombatHurt( kind : int, damage : float, maxHealth : float, attacker : int, flags : int ) : float;
//                                                                 // returns the Minecraft damage it becomes
//   import function WCraft_CombatPop() : int;                     // next MC event type (0 = none): 1 hit, 2 player died, 3 explosion
//   import function WCraft_CombatEvInt( i : int ) : int;          // current event: 0 id, 1 flags, 2 weapon
//   import function WCraft_CombatEvVec() : Vector;                // current event: see combat::Event x/y/z/w
//   import function WCraft_CombatParam( key : string ) : float;   // ini: "enabled" "range" "hitType" "deathMode" "hitScale" "hurtScale"
//   import function WCraft_CombatStatus() : string;
#include "combat.hpp"
#include "log.hpp"
#include "red_engine.hpp"
#include "skycraft_host.hpp"

#include <atomic>
#include <cstring>
#include <string>

namespace wc::natives
{
using red::CScriptStackFrame;
using red::FinishParams;
using red::ReadParam;
using red::RedVector;

namespace
{
std::atomic<long> g_actorCalls{0}, g_hurtCalls{0};
combat::Event g_current; // script thread only

int32_t ReadInt(CScriptStackFrame* s)
{
    alignas(16) uint8_t buf[32]{};
    ReadParam(s, buf);
    int32_t v;
    std::memcpy(&v, buf, sizeof(v));
    return v;
}

float ReadFloat(CScriptStackFrame* s)
{
    alignas(16) uint8_t buf[32]{};
    ReadParam(s, buf);
    float v;
    std::memcpy(&v, buf, sizeof(v));
    return v;
}

// WCraft_CombatActors( data : string ) : int
void N_CombatActors(void*, CScriptStackFrame* s, void* result)
{
    const std::string data = red::ReadStringParam(s);
    FinishParams(s);
    host::Start();
    const int n = combat::SetActors(data);
    const long c = ++g_actorCalls;
    if (c <= 5 || (n > 0 && c % 600 == 0))
        WLOG("WCraft_CombatActors call #%ld: %d actors (%zu bytes) %s", c, n, data.size(), data.substr(0, 160).c_str());
    if (result)
        *static_cast<int32_t*>(result) = n;
}

// WCraft_CombatHurt( kind : int, damage : float, maxHealth : float, attacker : int, flags : int ) : float
void N_CombatHurt(void*, CScriptStackFrame* s, void* result)
{
    const int32_t kind = ReadInt(s);
    const float damage = ReadFloat(s);
    const float maxHealth = ReadFloat(s);
    const int32_t attacker = ReadInt(s);
    const int32_t flags = ReadInt(s);
    FinishParams(s);
    const float mc = combat::Hurt(kind, damage, maxHealth, attacker, static_cast<uint32_t>(flags));
    if (++g_hurtCalls <= 20)
        WLOG("WCraft_CombatHurt(kind %d, %.1f of %.0f, attacker %d, flags %d) -> %.2f Minecraft", kind, damage, maxHealth, attacker, flags, mc);
    if (result)
        *static_cast<float*>(result) = mc;
}

// WCraft_CombatPop() : int
void N_CombatPop(void*, CScriptStackFrame* s, void* result)
{
    FinishParams(s);
    combat::Event e;
    int32_t type = 0;
    if (combat::Pop(e))
    {
        g_current = e;
        type = static_cast<int32_t>(e.type);
    }
    if (result)
        *static_cast<int32_t*>(result) = type;
}

// WCraft_CombatEvInt( i : int ) : int
void N_CombatEvInt(void*, CScriptStackFrame* s, void* result)
{
    const int32_t i = ReadInt(s);
    FinishParams(s);
    int32_t v = 0;
    if (i == 0)
        v = g_current.id;
    else if (i == 1)
        v = static_cast<int32_t>(g_current.flags);
    else if (i == 2)
        v = static_cast<int32_t>(g_current.weapon);
    if (result)
        *static_cast<int32_t*>(result) = v;
}

// WCraft_CombatEvVec() : Vector
void N_CombatEvVec(void*, CScriptStackFrame* s, void* result)
{
    FinishParams(s);
    if (result)
        *static_cast<RedVector*>(result) = {g_current.x, g_current.y, g_current.z, g_current.w};
}

// WCraft_CombatParam( key : string ) : float
void N_CombatParam(void*, CScriptStackFrame* s, void* result)
{
    const std::string key = red::ReadStringParam(s);
    FinishParams(s);
    const auto& c = combat::GetConfig();
    float v = 0.0f;
    if (key == "enabled")
        v = c.enabled ? 1.0f : 0.0f;
    else if (key == "range")
        v = c.range;
    else if (key == "hitType")
        v = static_cast<float>(c.hitType);
    else if (key == "deathMode")
        v = static_cast<float>(c.deathMode);
    else if (key == "hitScale")
        v = c.hitScale;
    else if (key == "hurtScale")
        v = c.hurtScale;
    if (result)
        *static_cast<float*>(result) = v;
}

// WCraft_CombatStatus() : string
void N_CombatStatus(void*, CScriptStackFrame* s, void* result)
{
    FinishParams(s);
    red::AssignString(result, "WitcherCraft" + combat::StatusSuffix());
}
} // namespace

int RegisterCombatNatives()
{
    int ok = 0;
    ok += red::RegisterGlobalNative("WCraft_CombatActors", &N_CombatActors) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_CombatHurt", &N_CombatHurt) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_CombatPop", &N_CombatPop) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_CombatEvInt", &N_CombatEvInt) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_CombatEvVec", &N_CombatEvVec) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_CombatParam", &N_CombatParam) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_CombatStatus", &N_CombatStatus) ? 1 : 0;
    WLOG("registered %d/7 combat natives (M4a)", ok);
    return ok;
}
} // namespace wc::natives
