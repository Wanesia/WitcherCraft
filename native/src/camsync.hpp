#pragma once
// Visual sync between the Witcher frame and Minecraft's world layer.
//
// The W3 camera is placed by the script once per game frame (control::OnFrame), usually with the
// pose of a Minecraft overlay frame F. W3 renders that frame on its render thread and presents it
// some frames later. To keep Minecraft's blocks glued to the world, the compositor must draw, at
// each Present, the Minecraft frame whose pose W3 actually rendered that image with - not the
// newest one. This module keeps:
//   - a log of game frames (game-frame number -> camera pose / Minecraft frame id),
//   - a log of Presents (present number -> how far the script was ahead),
//   - the latency estimate k: Present P shows game frame  P + d(P) - k,  d = script frames - presents
//     (robust mode over a window, so thread-timing races don't flip it). k is either fixed (ini
//     OverlayLatencyFrames) or measured: the overlay's image probe measures how far the Witcher
//     picture shifted horizontally at every Present, and k is the lag at which that shift
//     correlates best with the commanded camera yaw changes.
//   - rate statistics (presents / game frames / Minecraft frames), frame-generation detection and
//     user-facing warnings.
// Threads: OnGameFrame = script thread; OnPresent / OnProbeShift = the presenting thread;
// the rest any thread. Internally locked.
#include <cstdint>
#include <string>

namespace wc::camsync
{
// A camera in Minecraft's convention (x, y up, z; yaw 0 = +Z, pitch + = down, vertical FOV).
struct CamPose
{
    uint64_t frameId = 0; // Minecraft overlay frame the pose came from (0 = not from a frame)
    double eye[3] = {};
    float yaw = 0, pitch = 0, roll = 0, fov = 0;
};

// Script thread, once per game frame, after the W3 camera was placed. driving = the W3 camera is
// ours this frame (Minecraft mode engaged with a valid eye); pose = what it was placed with.
void OnGameFrame(bool driving, const CamPose& pose);
uint64_t GameFrames();

struct PresentInfo
{
    uint64_t present = 0;   // our present number (primary swapchain)
    uint64_t gameFrame = 0; // the game frame this image was (estimated to be) rendered in
    bool driving = false;   // the camera was ours in that frame
    bool havePose = false;
    CamPose pose;           // ... and placed with this pose
    int latency = 0;        // k in use
    bool frameGen = false;  // presents run ~2x faster than game frames
};
// The presenting thread, once per Present of the swapchain we composite into.
PresentInfo OnPresent();

// The presenting thread: the image probe's horizontal shift of present `present` against the
// previous one (pixels, + = the picture moved right), or valid=false.
void OnProbeShift(uint64_t present, bool valid, float shiftPx);

// The overlay worker: a Minecraft frame arrived (for the MC frame rate).
void OnMcFrame();

// What the compositor did at the last Present (for the statistics).
enum class Pick
{
    Exact,   // the frame W3 rendered with
    Nearest, // that frame is gone / unknown: the closest pose
    Newest,  // no lock possible (Witcher mode, HUD-only, disabled)
};
void OnPicked(Pick how, float poseErrDeg);

// Knobs (ini [WitcherCraft] in witchercraft_native.ini; also console wclat(n)).
//   OverlayLatencyFrames=-1   ; -1 = measure (image probe), else a fixed k
//   OverlayFrameMatch=1       ; 0 = old behaviour (always the newest Minecraft frame)
void SetLatency(int k);   // -1 auto
int LatencySetting();
int LatencyInUse();
void SetFrameMatch(bool on);
bool FrameMatch();

// The Witcher's own graphics settings (dx12user.settings), for the warnings.
struct GameSettings
{
    bool known = false;
    bool frameGen = false; // DLSSGMode / FSRFramegen / XessFrameGeneration
    bool motionBlur = false;
    bool blur = false;
    int limitFps = 0;
    bool vsync = false;
};
void SetGameSettings(const GameSettings& s);

// A one-line status (also logged every 10 s by OnPresent) and the next user-facing warning
// (consumed; "" = none). The script shows warnings as notifications.
std::string StatusLine();
std::string ConsumeWarning();

// Offline test: latency of the measured estimate (-1 = none yet), its correlation.
struct ProbeResult
{
    int k = -1;
    float corr = 0, runnerUp = 0;
    uint64_t samples = 0;
};
ProbeResult LastProbe();
} // namespace wc::camsync
