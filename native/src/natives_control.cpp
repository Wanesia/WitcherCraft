// WitcherCraft M3a natives: Minecraft drives the player. See control.hpp.
//
// WitcherScript side (witchercraft.ws imports):
//   import function WCraft_ControlFrame( flags : int, camHeading : float, camPitch : float ) : Vector;
//       once per frame. flags: 1 suspend (scene/vehicle/dead/fade), 2 no capture (W3 menu).
//       camHeading/camPitch: the current W3 camera look (pitch + = up), adopted when Minecraft takes over.
//       Returns the camera position (W3 coords); W = 1 Minecraft drives and the eye is valid,
//       2 Minecraft drives but no eye yet, 0 Witcher mode / not linked / suspended.
//   import function WCraft_ControlLook() : Vector;    // (pitch up+, heading, roll, FOV for the W3 camera (converted
//                                                     //  with CameraFovMode; 0 = leave the FOV alone)) of the last frame
//   import function WCraft_CameraOption( fovMode : int, usePose : int ) : string;  // calibration, -1 = unchanged
//   import function WCraft_ControlFeet() : Vector;    // interpolated Minecraft feet (W3 coords), W = 1 when Geralt
//                                                     // should be moved there (engaged, no teleport in flight)
//   import function WCraft_ControlMode() : bool;      // the F8 toggle (true = Minecraft mode)
//   import function WCraft_SetControlMode( on : bool );
//   import function WCraft_ControlNote( msg : string ); // a line in witchercraft_native.log (script diagnostics)
//   import function WCraft_SampleCentre() : Vector;   // where to sample ground heights (W3 coords), W = 1 valid
//   (witchercraft_interact.ws)
//   import function WCraft_InteractPoll() : Vector;   // X presses of the interact key (G) since the last poll,
//                                                     // Y of those handed to W3 as its interaction key (E),
//                                                     // Z 1 = held now, W the key (DIK)
//   import function WCraft_ControlBody() : Vector;    // the PLAYER's look: X heading (W3), Y pitch up, Z eye
//                                                     // height above the feet, W 1 = valid (Minecraft drives)
//   (witchercraft_control.ws, M6a mode identity)
//   import function WCraft_ModeRequest() : int;       // the Witcher panel key (DIK: 0x32 M, 0x24 J, 0x17 I, 0x25 K,
//                                                     // 0x31 N) that just switched Minecraft mode -> Witcher mode;
//                                                     // the script opens that panel once Geralt is back. 0 = none.
//                                                     // Consumed. Negative: -1 = McIdentity=0 (M3 key map)
//   import function WCraft_Panel( op : int, pos : Vector ) : int;  // the Witcher's panels as Steve
//                                                     // (M/J/I/K): op 1 = opened now, Geralt at pos;
//                                                     // 0 = closed / couldn't open; 2 = query. Returns
//                                                     // 1 panel state on, 2 the script was paused
//                                                     // meanwhile (a Witcher menu was up), 4 opened
//   import function WCraft_SensesHeld() : bool;       // M7: V held in Minecraft mode (Witcher senses)
//   import function WCraft_Options() : int;           // Minecraft's WitcherCraft options (kWc* bits, 0 = none)
//   import function WCraft_KeyName( which : int ) : string; // the key bound to 0 interact, 1 Steve/Geralt switch,
//                                                     // 2 Witcher senses, 3 Call Roach ("F8", "G"; "none")
#include "camsync.hpp"
#include "control.hpp"
#include "log.hpp"
#include "overlay_dx12.hpp"
#include "red_engine.hpp"
#include "skycraft_host.hpp"
#include "skycraft_protocol.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
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
std::atomic<long> g_frames{0}, g_notes{0};

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

bool ReadBool(CScriptStackFrame* s)
{
    alignas(16) uint8_t buf[32]{};
    ReadParam(s, buf);
    return buf[0] != 0;
}

void Put(void* result, const RedVector& v)
{
    if (result)
        *static_cast<RedVector*>(result) = v;
}

// WCraft_ControlFrame( flags : int, camHeading : float, camPitch : float ) : Vector
void N_ControlFrame(void*, CScriptStackFrame* s, void* result)
{
    const int32_t flags = ReadInt(s);
    const float heading = ReadFloat(s);
    const float pitch = ReadFloat(s);
    FinishParams(s);
    host::Start();
    const control::Frame f = control::OnFrame(static_cast<uint32_t>(flags), heading, pitch);
    const long n = ++g_frames;
    if (n == 1 || n % 20000 == 0)
        WLOG("WCraft_ControlFrame call #%ld flags 0x%X -> engaged %d eye %d (%.2f %.2f %.2f) hdg %.1f pitch %.1f", n, flags, f.engaged ? 1 : 0,
             f.eyeValid ? 1 : 0, f.eye[0], f.eye[1], f.eye[2], f.headingDeg, f.pitchUpDeg);
    RedVector v{0, 0, 0, 0};
    if (f.engaged)
        v = {static_cast<float>(f.eye[0]), static_cast<float>(f.eye[1]), static_cast<float>(f.eye[2]), f.eyeValid ? 1.0f : 2.0f};
    Put(result, v);
}

// WCraft_ControlLook() : Vector   (pitch up+, heading, roll, fov)
void N_ControlLook(void*, CScriptStackFrame* s, void* result)
{
    FinishParams(s);
    const control::Frame f = control::LastFrame();
    Put(result, {f.pitchUpDeg, f.headingDeg, f.rollDeg, f.fovDeg});
}

// WCraft_ControlFeet() : Vector
void N_ControlFeet(void*, CScriptStackFrame* s, void* result)
{
    FinishParams(s);
    const control::Frame f = control::LastFrame();
    RedVector v{0, 0, 0, 0};
    if (f.eyeValid && f.engaged && f.arrived) // only then may Geralt be moved there
        v = {static_cast<float>(f.feet[0]), static_cast<float>(f.feet[1]), static_cast<float>(f.feet[2]), 1.0f};
    Put(result, v);
}

// WCraft_ControlMode() : bool
void N_ControlMode(void*, CScriptStackFrame* s, void* result)
{
    FinishParams(s);
    if (result)
        *static_cast<bool*>(result) = control::McMode();
}

// WCraft_SetControlMode( on : bool )
void N_SetControlMode(void*, CScriptStackFrame* s, void*)
{
    const bool on = ReadBool(s);
    FinishParams(s);
    control::SetMcMode(on);
}

// WCraft_ControlNote( msg : string )
void N_ControlNote(void*, CScriptStackFrame* s, void*)
{
    std::string msg = red::ReadStringParam(s);
    FinishParams(s);
    if (++g_notes < 2000)
        WLOG("script: %s", msg.c_str());
}

// WCraft_SampleCentre() : Vector
void N_SampleCentre(void*, CScriptStackFrame* s, void* result)
{
    FinishParams(s);
    float x = 0, y = 0, z = 0;
    RedVector v{0, 0, 0, 0};
    if (host::SampleCentre(x, y, z))
        v = {x, y, z, 1.0f};
    Put(result, v);
}
// WCraft_CameraOption( fovMode : int, usePose : int ) : string   (-1 = leave as is)
void N_CameraOption(void*, CScriptStackFrame* s, void* result)
{
    const int32_t fovMode = ReadInt(s);
    const int32_t usePose = ReadInt(s);
    FinishParams(s);
    if (fovMode >= 0)
        control::SetFovMode(fovMode);
    if (usePose >= 0)
        control::SetUsePose(usePose != 0);
    const control::Frame f = control::LastFrame();
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "FOV mode %d (0 vertical, 1 horizontal real aspect, 2 horizontal 4:3): MC %.1f -> W3 %.1f | camera source %s (this frame: %s)",
                  control::FovMode(), f.mcFovDeg, f.fovDeg, control::UsePose() ? "overlay pose" : "interpolation",
                  f.fromPose ? "pose" : "interpolation");
    red::AssignString(result, buf);
}

// WCraft_SyncOption( latency : int, frameMatch : int ) : string
//   latency: -2 = unchanged, -1 = measure (image probe), 0..8 = fixed; frameMatch: -1 unchanged, 0/1.
//   Returns the sync status line.
void N_SyncOption(void*, CScriptStackFrame* s, void* result)
{
    const int32_t latency = ReadInt(s);
    const int32_t match = ReadInt(s);
    FinishParams(s);
    if (latency >= -1)
        camsync::SetLatency(latency);
    if (match >= 0)
        camsync::SetFrameMatch(match != 0);
    red::AssignString(result, camsync::StatusLine());
}

// WCraft_SyncWarning() : string   ("" = nothing new; the script shows it as a notification)
void N_SyncWarning(void*, CScriptStackFrame* s, void* result)
{
    FinishParams(s);
    red::AssignString(result, camsync::ConsumeWarning());
}

// WCraft_InteractPoll() : Vector
void N_InteractPoll(void*, CScriptStackFrame* s, void* result)
{
    FinishParams(s);
    const control::InteractPoll p = control::ConsumeInteract();
    if (p.presses)
        WLOG("interact: %u press(es) of the interact key, %u handed to The Witcher as its interaction key", p.presses, p.givenToW3);
    Put(result, {static_cast<float>(p.presses), static_cast<float>(p.givenToW3), p.held ? 1.0f : 0.0f, static_cast<float>(p.dik)});
}

// WCraft_ControlBody() : Vector
void N_ControlBody(void*, CScriptStackFrame* s, void* result)
{
    FinishParams(s);
    const control::Frame f = control::LastFrame();
    Put(result, {f.bodyHeadingDeg, f.lookPitchUpDeg, f.eyeHeight, (f.engaged && f.eyeValid) ? 1.0f : 0.0f});
}

// WCraft_ModeRequest() : int
void N_ModeRequest(void*, CScriptStackFrame* s, void* result)
{
    FinishParams(s);
    int32_t v = static_cast<int32_t>(control::ConsumeW3MenuRequest());
    if (!v && !control::IdentityKeys())
        v = -1;
    if (v > 0)
        WLOG("identity: script asked for the pending Witcher panel (DIK 0x%02X)", static_cast<unsigned>(v));
    if (result)
        std::memcpy(result, &v, sizeof(v));
}

// WCraft_Panel( op : int, pos : Vector ) : int   (the Witcher's panels as Steve)
void N_Panel(void*, CScriptStackFrame* s, void* result)
{
    const int32_t op = ReadInt(s);
    RedVector pos{0, 0, 0, 1};
    ReadParam(s, &pos);
    FinishParams(s);
    if (op == 1)
        control::PanelBegin(pos.x, pos.y, pos.z);
    else if (op == 0)
        control::PanelEnd("the script saw the Witcher's menu close");
    const int32_t v = static_cast<int32_t>(control::PanelState());
    if (result)
        std::memcpy(result, &v, sizeof(v));
}

// WCraft_SensesHeld() : bool   (M7: V held in Minecraft mode = Witcher senses)
void N_SensesHeld(void*, CScriptStackFrame* s, void* result)
{
    FinishParams(s);
    if (result)
        *static_cast<bool*>(result) = control::SensesHeld();
}

// WCraft_Options() : int   Minecraft's WitcherCraft options (proto::kWc* bits; 0 = an older
// Minecraft: the script keeps its defaults). Also applies "walls hide blocks" when it changes.
void N_Options(void*, CScriptStackFrame* s, void* result)
{
    FinishParams(s);
    namespace proto = skycraft::proto;
    const uint32_t f = control::WcFlags();
    static int s_occ = -1;
    if (f & proto::kWcValid)
    {
        const int occ = (f & proto::kWcOcclusion) ? 1 : 0;
        if (occ != s_occ)
        {
            if (s_occ != -1 || !occ) // first sight: only act if the option turns it off
                overlay::OcclusionCommand(1, static_cast<float>(occ));
            WLOG("options: walls hide blocks %s (Minecraft's WitcherCraft options)", occ ? "ON" : "OFF");
            s_occ = occ;
        }
    }
    const int32_t v = static_cast<int32_t>(f);
    if (result)
        std::memcpy(result, &v, sizeof(v));
}

// WCraft_KeyName( which : int ) : string   for the key hints (Minecraft's WitcherCraft controls)
void N_KeyName(void*, CScriptStackFrame* s, void* result)
{
    const int32_t which = ReadInt(s);
    FinishParams(s);
    red::AssignString(result, control::KeyName(control::KeyDik(static_cast<uint32_t>(which))));
}

// WCraft_PollLights() : string   Minecraft's light-emitting blocks near the player (protocol
// WcLights): "" if unchanged since the last call, else "version;x,y,z,level,kind;..." with x/y/z
// the block centre in W3 coordinates. kind: 0 warm, 1 soul, 2 lava, 3 white, 4 red.
void N_PollLights(void*, CScriptStackFrame* s, void* result)
{
    FinishParams(s);
    namespace proto = skycraft::proto;
    static uint32_t s_lastVersion = 0;
    std::string out;
    uint8_t* base = host::MappingBase();
    if (base)
    {
        auto* src = reinterpret_cast<proto::WcLights*>(base + proto::kOffWcLights);
        auto* seqp = reinterpret_cast<std::atomic<uint32_t>*>(&src->seq);
        for (int attempt = 0; attempt < 64; ++attempt)
        {
            const uint32_t s1 = seqp->load(std::memory_order_acquire);
            if (s1 & 1)
                continue;
            proto::WcLights copy;
            std::memcpy(&copy, src, sizeof(copy));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seqp->load(std::memory_order_relaxed) != s1)
                continue;
            if (s1 == 0 || copy.version == s_lastVersion)
                break;
            s_lastVersion = copy.version;
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%u;", copy.version);
            out = buf;
            const uint32_t n = std::min(copy.count, proto::kMaxWcLights);
            for (uint32_t i = 0; i < n; ++i)
            {
                const auto& l = copy.lights[i];
                // MC block -> W3: x = x, y = -z, z = y (block centre).
                std::snprintf(buf, sizeof(buf), "%.1f,%.1f,%.1f,%u,%u;", l.x + 0.5, -(l.z + 0.5), l.y + 0.5, l.level, l.kind);
                out += buf;
            }
            WLOG("lights: %u light-emitting Minecraft blocks near the player (version %u)", n, copy.version);
            break;
        }
    }
    red::AssignString(result, out);
}

// The overlay's world-layer pose, adapted for control.cpp (the offline tools link no overlay).
bool PoseFromOverlay(control::OverlayPose& out)
{
    overlay::Pose p;
    if (!overlay::LastDrawnPose(p))
        return false;
    out.frameId = p.frameId;
    out.takenMs = p.takenMs;
    out.eye[0] = p.eye[0], out.eye[1] = p.eye[1], out.eye[2] = p.eye[2];
    out.yaw = p.yaw, out.pitch = p.pitch, out.fovDeg = p.fovDeg, out.rollDeg = p.rollDeg;
    return true;
}
} // namespace

int RegisterControlNatives()
{
    control::SetPoseProvider(&PoseFromOverlay);
    int ok = 0;
    ok += red::RegisterGlobalNative("WCraft_CameraOption", &N_CameraOption) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_ControlFrame", &N_ControlFrame) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_ControlLook", &N_ControlLook) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_ControlFeet", &N_ControlFeet) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_ControlMode", &N_ControlMode) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_SetControlMode", &N_SetControlMode) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_ControlNote", &N_ControlNote) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_SampleCentre", &N_SampleCentre) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_SyncOption", &N_SyncOption) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_SyncWarning", &N_SyncWarning) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_InteractPoll", &N_InteractPoll) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_ControlBody", &N_ControlBody) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_ModeRequest", &N_ModeRequest) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_SensesHeld", &N_SensesHeld) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_Options", &N_Options) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_KeyName", &N_KeyName) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_PollLights", &N_PollLights) ? 1 : 0;
    ok += red::RegisterGlobalNative("WCraft_Panel", &N_Panel) ? 1 : 0;
    WLOG("registered %d/18 control natives (M3a + sync + interact + identity + senses + options + lights + panels)", ok);
    return ok;
}
} // namespace wc::natives
