// WitcherCraft M2b natives: Minecraft blocks in W3 and build mode.
//
// WitcherScript side (witchercraft_natives.ws):
//   import function WCraft_PollBlocks( max : int ) : string;   // "+id,x,y,z,class;" / "-id;" (W3 block min corner)
//   import function WCraft_ClearBlocks();                      // script destroyed all block entities
//   import function WCraft_BuildMode() : bool;                 // build mode on? (F7 toggles natively)
//   import function WCraft_SetBuildMode( on : bool );
//   import function WCraft_SetAim( active : bool, camPos : Vector, camDir : Vector, hit : Vector ) : Vector;
//                                                              // hit.W = 1 if the W3 trace hit; returns aim point, W = 1 if something was hit
//   import function WCraft_McCommand( command : string );      // a Minecraft chat command, e.g. "gamemode creative"
//   import function WCraft_BlockStatus() : string;
#include "blocks.hpp"
#include "buildmode.hpp"
#include "collision.hpp"
#include "log.hpp"
#include "red_engine.hpp"
#include "skycraft_host.hpp"

#include <atomic>
#include <cstring>

namespace wc::natives
{
using red::CScriptStackFrame;
using red::FinishParams;
using red::ReadParam;
using red::RedVector;

namespace
{
std::atomic<long> g_pollCalls{0}, g_aimCalls{0};

int32_t ReadInt(CScriptStackFrame* s)
{
    alignas(16) uint8_t buf[32]{};
    ReadParam(s, buf);
    int32_t v;
    std::memcpy(&v, buf, sizeof(v));
    return v;
}

bool ReadBool(CScriptStackFrame* s)
{
    alignas(16) uint8_t buf[32]{};
    ReadParam(s, buf);
    return buf[0] != 0;
}

RedVector ReadVector(CScriptStackFrame* s)
{
    alignas(16) uint8_t buf[64]{};
    ReadParam(s, buf);
    RedVector v;
    std::memcpy(&v, buf, sizeof(v));
    return v;
}

// WCraft_PollBlocks( max : int ) : string
void N_PollBlocks(void*, CScriptStackFrame* s, void* result)
{
    const int32_t max = ReadInt(s);
    FinishParams(s);
    std::string out = blocks::Poll(max < 0 ? 0 : (max > 256 ? 256 : max));
    const long n = ++g_pollCalls;
    if (!out.empty() && (n < 50 || n % 500 == 0))
        WLOG("WCraft_PollBlocks(%d) call #%ld -> %s", max, n, out.size() > 200 ? (out.substr(0, 200) + "...").c_str() : out.c_str());
    red::AssignString(result, out);
}

// WCraft_ClearBlocks()
void N_ClearBlocks(void*, CScriptStackFrame* s, void*)
{
    FinishParams(s);
    blocks::ResetSpawned();
}

// WCraft_BuildMode() : bool
void N_BuildMode(void*, CScriptStackFrame* s, void* result)
{
    FinishParams(s);
    host::Start();
    if (result)
        *static_cast<bool*>(result) = buildmode::Enabled();
}

// WCraft_SetBuildMode( on : bool )
void N_SetBuildMode(void*, CScriptStackFrame* s, void*)
{
    const bool on = ReadBool(s);
    FinishParams(s);
    buildmode::SetEnabled(on);
}

// WCraft_SetAim( active : bool, camPos : Vector, camDir : Vector, hit : Vector ) : Vector
void N_SetAim(void*, CScriptStackFrame* s, void* result)
{
    const bool active = ReadBool(s);
    const RedVector cam = ReadVector(s);
    const RedVector dir = ReadVector(s);
    const RedVector hit = ReadVector(s);
    FinishParams(s);
    const float c[3] = {cam.x, cam.y, cam.z}, d[3] = {dir.x, dir.y, dir.z}, h[3] = {hit.x, hit.y, hit.z};
    const auto r = buildmode::SetAim(active, c, d, h, hit.w > 0.5f);
    const long n = ++g_aimCalls;
    if (active && (n <= 5 || n % 2000 == 0))
        WLOG("WCraft_SetAim call #%ld cam (%.2f %.2f %.2f) dir (%.2f %.2f %.2f) hit %.0f -> (%.2f %.2f %.2f) %.0f", n, cam.x, cam.y, cam.z, dir.x,
             dir.y, dir.z, hit.w, r.x, r.y, r.z, r.w);
    if (result)
        *static_cast<RedVector*>(result) = {r.x, r.y, r.z, r.w};
}

// WCraft_McCommand( command : string )
void N_McCommand(void*, CScriptStackFrame* s, void*)
{
    std::string cmd = red::ReadStringParam(s);
    FinishParams(s);
    WLOG("WCraft_McCommand(\"%s\")", cmd.c_str());
    buildmode::QueueCommand(cmd);
}

// WCraft_BlockStatus() : string
void N_BlockStatus(void*, CScriptStackFrame* s, void* result)
{
    FinishParams(s);
    red::AssignString(result, "WitcherCraft" + blocks::StatusSuffix() + buildmode::StatusSuffix());
}
} // namespace

int RegisterBlockNatives()
{
    int ok = 0;
    ok += red::RegisterGlobalNative("WCraft_PollBlocks", &N_PollBlocks) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_ClearBlocks", &N_ClearBlocks) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_BuildMode", &N_BuildMode) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_SetBuildMode", &N_SetBuildMode) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_SetAim", &N_SetAim) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_McCommand", &N_McCommand) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_BlockStatus", &N_BlockStatus) ? 1 : 0;
    WLOG("registered %d/7 block natives (M2b)", ok);
    collision::SetStandInSource(&blocks::StandInCells); // Steve's collision skips the blocks' collision boxes
    return ok;
}
} // namespace wc::natives
