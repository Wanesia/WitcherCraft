// M5a natives: W3 walls and buildings as Minecraft collision. See collision.hpp.
//
// WitcherScript side (witchercraft.ws):
//   import function WCraft_ColScan() : int;            // game thread, every script step: scans W3's
//                                                      // PhysX scene around Minecraft's player within
//                                                      // CollisionBudgetUs; columns scanned (-1 = off)
//   import function WCraft_ColStatus() : string;
//   import function WCraft_ColEnable( on : bool );     // console kill switch (ini: Collision=0)
#include "collision.hpp"
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

namespace
{
std::atomic<long> g_scanCalls{0};

// WCraft_ColScan() : int
void N_ColScan(void*, CScriptStackFrame* s, void* result)
{
    FinishParams(s);
    host::Start();
    float x, y, z;
    const int n = host::ScanCentre(x, y, z) ? collision::Scan(x, y, z) : 0; // M6b: unled centre (feet / teleport target)
    const long c = ++g_scanCalls;
    if (c <= 3)
        WLOG("WCraft_ColScan call #%ld -> %d", c, n);
    if (result)
        *static_cast<int32_t*>(result) = n;
}

// WCraft_ColStatus() : string
void N_ColStatus(void*, CScriptStackFrame* s, void* result)
{
    FinishParams(s);
    const std::string st = collision::NativeStatus();
    WLOG("WCraft_ColStatus: %s", st.c_str());
    if (result)
        red::AssignString(result, st);
}

// WCraft_ColEnable( on : bool )
void N_ColEnable(void*, CScriptStackFrame* s, void*)
{
    alignas(16) uint8_t buf[32]{};
    ReadParam(s, buf);
    FinishParams(s);
    collision::SetEnabled(buf[0] != 0);
}
} // namespace

int RegisterCollisionNatives()
{
    int ok = 0;
    ok += red::RegisterGlobalNative("WCraft_ColScan", &N_ColScan) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_ColStatus", &N_ColStatus) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_ColEnable", &N_ColEnable) ? 1 : 0;
    WLOG("registered %d/3 collision natives (M5a)", ok);
    return ok;
}
} // namespace wc::natives
