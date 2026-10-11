// WitcherCraft M5b natives: occlusion of Minecraft's world layer by Witcher geometry.
//
// WitcherScript side (witchercraft_visual.ws):
//   import function WCraft_Occlusion( cmd : int, value : float ) : string;
//     cmd 0 status, 1 on/off, 2 bias (m), 3 depth candidate (-1 auto), 4 near (m), 5 far (m, 0 = infinite),
//     6 reversed Z (-1 auto, 0, 1), 7 calibrate, 8 calibration reset, 9 debug view (0/1/2), 10 soft edge (m),
//     11 next candidate. Returns the occlusion status line (refreshed every 10 s).
#include "log.hpp"
#include "overlay_dx12.hpp"
#include "red_engine.hpp"

#include <cstring>

namespace wc::natives
{
namespace
{
using red::CScriptStackFrame;

int32_t ReadIntParam(CScriptStackFrame* s)
{
    alignas(16) uint8_t buf[32]{};
    red::ReadParam(s, buf);
    int32_t v;
    std::memcpy(&v, buf, sizeof(v));
    return v;
}

float ReadFloatParam(CScriptStackFrame* s)
{
    alignas(16) uint8_t buf[32]{};
    red::ReadParam(s, buf);
    float v;
    std::memcpy(&v, buf, sizeof(v));
    return v;
}

// WCraft_Occlusion( cmd : int, value : float ) : string
void N_Occlusion(void*, CScriptStackFrame* s, void* result)
{
    const int32_t cmd = ReadIntParam(s);
    const float value = ReadFloatParam(s);
    red::FinishParams(s);
    red::AssignString(result, overlay::OcclusionCommand(cmd, value));
}
} // namespace

int RegisterVisualNatives()
{
    const int ok = red::RegisterGlobalNative("WCraft_Occlusion", &N_Occlusion) ? 1 : 0;
    WLOG("registered %d/1 visual natives (M5b occlusion)", ok);
    return ok;
}
} // namespace wc::natives
