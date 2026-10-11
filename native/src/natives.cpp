// WitcherCraft script natives.
//
// WitcherScript side (mods\modWitcherCraft\content\scripts\local\witchercraft_natives.ws):
//   import function WCraft_Ping() : int;
//   import function WCraft_GetBlockCount() : int;
//   import function WCraft_SetPlayerPos( pos : Vector ) : bool;
//   import function WCraft_GetLastPos() : Vector;
//   import function WCraft_Echo( msg : string ) : string;
//   import function WCraft_Rpc( request : string ) : string;
//
// SkyCraft host (M1, mods\modWitcherCraft\content\scripts\local\witchercraft.ws):
//   import function WCraft_HostTick( pos : Vector, headingDeg : float, pitchDeg : float, flags : int ) : bool;
//   import function WCraft_PushHeightColumn( x : int, y : int, groundZ : float );
//   import function WCraft_PushHeights( data : string ) : int;      // "x,y,h;x,y,h;..." ('n' = no ground)
//   import function WCraft_SetWorld( worldName : string );
//   import function WCraft_GetMcStatus() : string;
//   import function WCraft_GetMcPos() : Vector;                     // MC player in W3 coords, W=1 if valid
//
// A native is `void fn(IScriptable* ctx, CScriptStackFrame* stack, void* result)`.
// Params MUST be consumed in declaration order with ReadParam(), then FinishParams()
// exactly once, even if the result is discarded (result == nullptr).
#include "natives.hpp"

#include "config.hpp"
#include "hook.hpp"
#include "log.hpp"
#include "red_engine.hpp"
#include "skycraft_host.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>

namespace wc::natives
{
using red::CScriptStackFrame;
using red::FinishParams;
using red::ReadParam;
using red::RedVector;

namespace
{
constexpr int32_t kPingMagic = 1337;
constexpr char kVersion[] = "witchercraft-native " WCRAFT_VERSION;

std::mutex g_posMutex;
RedVector g_lastPos{0, 0, 0, 1};

struct CallCounter
{
    std::atomic<long> n{0};
    bool ShouldLog()
    {
        long v = ++n;
        return v <= g_config.verboseCalls || (v % 1000) == 0;
    }
};
CallCounter c_ping, c_blocks, c_setpos, c_getpos, c_echo, c_rpc;
CallCounter c_tick, c_col, c_heights, c_world, c_status, c_mcpos;

// Typed param readers. The opcode handler writes the value into the buffer we hand it; use an
// aligned, oversized scratch buffer so a wider-than-expected write can't hurt the stack.
float ReadFloatParam(CScriptStackFrame* s)
{
    alignas(16) uint8_t buf[32]{};
    ReadParam(s, buf);
    float v;
    std::memcpy(&v, buf, sizeof(v));
    return v;
}

int32_t ReadIntParam(CScriptStackFrame* s)
{
    alignas(16) uint8_t buf[32]{};
    ReadParam(s, buf);
    int32_t v;
    std::memcpy(&v, buf, sizeof(v));
    return v;
}

// WCraft_Ping() : int
void N_Ping(void*, CScriptStackFrame* s, void* result)
{
    FinishParams(s);
    if (c_ping.ShouldLog())
        WLOG("WCraft_Ping() call #%ld -> %d", c_ping.n.load(), kPingMagic);
    if (result)
        *static_cast<int32_t*>(result) = kPingMagic;
}

// WCraft_GetBlockCount() : int   -- collision blocks streamed to Minecraft so far (-1: no host)
void N_GetBlockCount(void*, CScriptStackFrame* s, void* result)
{
    FinishParams(s);
    const auto st = host::GetStatus();
    const int32_t v = st.mapped ? static_cast<int32_t>(std::min<uint64_t>(st.blocksSent, INT32_MAX)) : -1;
    if (c_blocks.ShouldLog())
        WLOG("WCraft_GetBlockCount() call #%ld -> %d", c_blocks.n.load(), v);
    if (result)
        *static_cast<int32_t*>(result) = v;
}

// WCraft_SetPlayerPos( pos : Vector ) : bool
void N_SetPlayerPos(void*, CScriptStackFrame* s, void* result)
{
    RedVector pos{0, 0, 0, 1};
    ReadParam(s, &pos);
    FinishParams(s);
    {
        std::lock_guard lk(g_posMutex);
        g_lastPos = pos;
    }
    if (c_setpos.ShouldLog())
        WLOG("WCraft_SetPlayerPos(%.3f, %.3f, %.3f, %.3f) call #%ld", pos.x, pos.y, pos.z, pos.w, c_setpos.n.load());
    if (result)
        *static_cast<bool*>(result) = true;
}

// WCraft_GetLastPos() : Vector
void N_GetLastPos(void*, CScriptStackFrame* s, void* result)
{
    FinishParams(s);
    RedVector v;
    {
        std::lock_guard lk(g_posMutex);
        v = g_lastPos;
    }
    if (c_getpos.ShouldLog())
        WLOG("WCraft_GetLastPos() call #%ld -> (%.3f, %.3f, %.3f)", c_getpos.n.load(), v.x, v.y, v.z);
    if (result)
        *static_cast<RedVector*>(result) = v;
}

// WCraft_Echo( msg : string ) : string
void N_Echo(void*, CScriptStackFrame* s, void* result)
{
    std::string msg = red::ReadStringParam(s);
    FinishParams(s);
    if (c_echo.ShouldLog())
        WLOG("WCraft_Echo(\"%s\") call #%ld", msg.c_str(), c_echo.n.load());
    red::AssignString(result, "WCraft:" + msg);
}

// WCraft_Rpc( request : string ) : string     -- generic text RPC
void N_Rpc(void*, CScriptStackFrame* s, void* result)
{
    std::string req = red::ReadStringParam(s);
    FinishParams(s);
    std::string reply = HandleRpc(req);
    if (c_rpc.ShouldLog())
        WLOG("WCraft_Rpc(\"%s\") -> \"%s\"", req.c_str(), reply.c_str());
    red::AssignString(result, reply);
}

// ---- SkyCraft host natives -----------------------------------------------------------------

// WCraft_HostTick( pos : Vector, headingDeg : float, pitchDeg : float, flags : int ) : bool
// Returns true while Minecraft is linked.
void N_HostTick(void*, CScriptStackFrame* s, void* result)
{
    RedVector pos{0, 0, 0, 1};
    ReadParam(s, &pos);
    const float heading = ReadFloatParam(s);
    const float pitch = ReadFloatParam(s);
    const int32_t flags = ReadIntParam(s);
    FinishParams(s);
    host::Tick(pos.x, pos.y, pos.z, heading, pitch, static_cast<uint32_t>(flags));
    const bool linked = host::GetStatus().mcAlive;
    if (c_tick.ShouldLog())
        WLOG("WCraft_HostTick((%.2f, %.2f, %.2f), hdg %.1f, pitch %.1f, flags 0x%X) call #%ld -> linked=%d", pos.x, pos.y, pos.z,
             heading, pitch, flags, c_tick.n.load(), linked ? 1 : 0);
    if (result)
        *static_cast<bool*>(result) = linked;
}

// WCraft_PushHeightColumn( x : int, y : int, groundZ : float )   -- W3 lattice point
void N_PushHeightColumn(void*, CScriptStackFrame* s, void*)
{
    const int32_t x = ReadIntParam(s);
    const int32_t y = ReadIntParam(s);
    const float h = ReadFloatParam(s);
    FinishParams(s);
    host::PushHeight(x, y, h);
    if (c_col.ShouldLog())
        WLOG("WCraft_PushHeightColumn(%d, %d, %.2f) call #%ld", x, y, h, c_col.n.load());
}

// WCraft_PushHeights( data : string ) : int    -- "x,y,h;..." returns samples accepted
void N_PushHeights(void*, CScriptStackFrame* s, void* result)
{
    std::string data = red::ReadStringParam(s);
    FinishParams(s);
    const int n = host::PushHeights(data);
    if (c_heights.ShouldLog())
        WLOG("WCraft_PushHeights(%zu bytes) call #%ld -> %d samples", data.size(), c_heights.n.load(), n);
    if (result)
        *static_cast<int32_t*>(result) = n;
}

// WCraft_SetWorld( worldName : string )
void N_SetWorld(void*, CScriptStackFrame* s, void*)
{
    std::string name = red::ReadStringParam(s);
    FinishParams(s);
    host::Start();
    host::SetWorld(name);
    if (c_world.ShouldLog())
        WLOG("WCraft_SetWorld(\"%s\") call #%ld", name.c_str(), c_world.n.load());
}

// WCraft_GetMcStatus() : string
void N_GetMcStatus(void*, CScriptStackFrame* s, void* result)
{
    FinishParams(s);
    const std::string line = host::StatusLine();
    if (c_status.ShouldLog())
        WLOG("WCraft_GetMcStatus() call #%ld -> %s", c_status.n.load(), line.c_str());
    red::AssignString(result, line);
}

// WCraft_GetMcPos() : Vector   -- MC player feet in W3 coordinates; W = 1 valid, 0 not
void N_GetMcPos(void*, CScriptStackFrame* s, void* result)
{
    FinishParams(s);
    const auto st = host::GetStatus();
    RedVector v{0, 0, 0, 0};
    if (st.mcAlive && st.haveMcState)
        v = {static_cast<float>(st.mcX), static_cast<float>(-st.mcZ), static_cast<float>(st.mcY), 1.0f};
    if (c_mcpos.ShouldLog())
        WLOG("WCraft_GetMcPos() call #%ld -> (%.2f, %.2f, %.2f, %.0f)", c_mcpos.n.load(), v.x, v.y, v.z, v.w);
    if (result)
        *static_cast<RedVector*>(result) = v;
}

struct NativeDef
{
    const char* name; // static storage (name pool may keep the pointer)
    red::ScriptNative fn;
};
const NativeDef kNatives[] = {
    {"WCraft_Ping", &N_Ping},
    {"WCraft_GetBlockCount", &N_GetBlockCount},
    {"WCraft_SetPlayerPos", &N_SetPlayerPos},
    {"WCraft_GetLastPos", &N_GetLastPos},
    {"WCraft_Echo", &N_Echo},
    {"WCraft_Rpc", &N_Rpc},
    {"WCraft_HostTick", &N_HostTick},
    {"WCraft_PushHeightColumn", &N_PushHeightColumn},
    {"WCraft_PushHeights", &N_PushHeights},
    {"WCraft_SetWorld", &N_SetWorld},
    {"WCraft_GetMcStatus", &N_GetMcStatus},
    {"WCraft_GetMcPos", &N_GetMcPos},
};

std::atomic<bool> g_done{false};
void (*g_origRegister)(void*, void*) = nullptr;

void RegisterAll()
{
    WLOG("registering WitcherCraft natives (slots used: %d / %u)", red::NativeSlotsUsed(), red::g.tableCapacity);
    int ok = 0;
    for (const auto& n : kNatives)
        ok += red::RegisterGlobalNative(n.name, n.fn) ? 1 : 0;
    WLOG("registered %d/%zu natives", ok, std::size(kNatives));
    RegisterBlockNatives();
    RegisterControlNatives();
    RegisterCombatNatives();
    RegisterCollisionNatives();
    RegisterVisualNatives();
    // Create the SkyCraft mapping now (engine init, main thread, not under the loader lock) so
    // Minecraft can link and open its world while the game is still loading.
    if (!host::Start())
        WLOG("SkyCraft host failed to start (see above); WCraft_HostTick will keep retrying nothing");
}

// Replaces ONE call: `call CRTTISystem::RegisterGlobalFunction` for RandDifferent,
// inside the engine's math-native export routine (runs once during engine init,
// before scripts are compiled).
void Hook_RegisterGlobalFunction(void* rtti, void* func)
{
    g_origRegister(rtti, func);
    if (!g_done.exchange(true))
        RegisterAll();
}
} // namespace

bool InstallRegistrationHook()
{
    void* orig = nullptr;
    if (!hook::RedirectCall(red::g.hookCallSite, reinterpret_cast<void*>(&Hook_RegisterGlobalFunction), &orig))
        return false;
    if (orig != reinterpret_cast<void*>(red::g.RegisterGlobalFunction))
    {
        WLOG("hook: unexpected original call target %p (expected %p)", orig,
             reinterpret_cast<void*>(red::g.RegisterGlobalFunction));
    }
    g_origRegister = reinterpret_cast<void (*)(void*, void*)>(orig);
    WLOG("hook: registration hook installed at %p", red::g.hookCallSite);
    return true;
}

std::string HandleRpc(const std::string& request)
{
    // Minimal text protocol: "<cmd>[ <args>]". Extend freely.
    if (request == "ping")
        return "pong";
    if (request == "version")
        return kVersion;
    if (request == "blocks")
    {
        const auto st = host::GetStatus();
        return st.mapped ? std::to_string(st.blocksSent) : "-1";
    }
    if (request == "shm" || request == "status")
        return host::StatusLine();
    if (request == "slots")
        return std::to_string(red::NativeSlotsUsed()) + "/" + std::to_string(red::g.tableCapacity);
    return "error:unknown-command";
}
} // namespace wc::natives
