#pragma once
// Minecraft HUD overlay for the DX12 Witcher 3.
//
// Minecraft renders its HUD/hand/screens offscreen and publishes RGBA8 premultiplied frames
// through the SkyCraft overlay triple buffer in Local\SkyCraft_v1. We composite the newest
// one over the game's back buffer right before the swapchain presents it.
//
// Hooks are vtable-entry swaps only (no code patching):
//   IDXGIFactory::CreateSwapChain / IDXGIFactory2::CreateSwapChainForHwnd  (installed at once;
//     tells us the native swapchain AND the queue it presents on, and gives us its vtables)
//   IDXGISwapChain::Present / Present1 / ResizeBuffers, IDXGISwapChain3::ResizeBuffers1 /
//     SetColorSpace1, ID3D12CommandQueue::ExecuteCommandLists
// Every D3D call is checked; any failure or SEH exception disables the overlay for the
// session (the hooks then only forward).
#include <cstdint>
#include <string>

namespace wc::overlay
{
// Starts the installer thread (no-op if Overlay=0 in the ini). Safe to call from DllMain.
void Start();

// Synchronous install (offline test). Always installs the factory hooks; with
// viaDummyDevice also hooks the swapchain/queue vtables through a throwaway device/swapchain
// (the fallback used when the game's swapchain already exists). False on failure.
bool InstallNow(bool viaDummyDevice);

// Debug/test: after the next overlay draw, copy the composited back buffer and write it as a
// 32-bit top-down BMP (HDR formats are converted back to sRGB with the same paper white).
void RequestCapture(const wchar_t* bmpPath);
bool CaptureDone();

struct Stats
{
    uint64_t presents = 0;       // Present/Present1 calls seen (all swapchains)
    uint64_t draws = 0;          // frames we composited the HUD into
    uint64_t uploads = 0;        // overlay frames copied to the GPU texture
    uint64_t acquired = 0;       // overlay frames taken from the triple buffer
    uint64_t skippedBusy = 0;    // frames skipped because our previous GPU work wasn't done
    uint32_t texW = 0, texH = 0; // overlay texture size
    uint32_t bbW = 0, bbH = 0;   // back buffer size
    int colorMode = 0;           // 0 SDR, 1 scRGB, 2 HDR10/PQ
    bool visible = false;        // last frame: HUD shown
    bool disabled = false;
};
Stats GetStats();

// M3a (control.cpp): the camera Minecraft rendered the newest overlay frame we took for drawing
// with (slot header, kOverlayHasPose). MC coordinates/degrees, bob and F5 included. Any thread.
struct Pose
{
    uint64_t frameId = 0;
    uint64_t takenMs = 0; // GetTickCount64() when the overlay worker took the frame
    double eye[3] = {};
    float yaw = 0, pitch = 0, fovDeg = 0, rollDeg = 0;
};
bool LastDrawnPose(Pose& out); // false: no world-layer frame with a pose yet

// Sync diagnostics / offline test: what the last Present composited.
struct Composited
{
    bool valid = false;
    uint64_t present = 0;   // camsync present number
    uint64_t gameFrame = 0; // the game frame that image was estimated to be rendered in
    int pick = 2;           // 0 exact (the frame W3 rendered with), 1 nearest pose, 2 newest frame
    uint64_t frameId = 0;   // Minecraft frame drawn
    Pose pose;              // ... and its camera
    bool camValid = false;
    Pose cam;               // the camera the script placed in that game frame (MC convention)
};
Composited LastComposited();

// Occlusion by Witcher geometry. Any thread. cmd: 0 status, 1 on/off (value),
// 2 bias (m), 3 depth candidate (-1 auto), 4 near (m), 5 far (m, 0 = infinite), 6 reversed Z
// (-1 auto/0/1), 7 calibrate (next frame), 8 calibration reset, 9 debug view (0/1/2), 10 soft (m),
// 11 next candidate. Returns the last status line (refreshed every 10 s).
std::string OcclusionCommand(int cmd, float value);
// Test/diagnostics: the W3 depth model in use (z = B / (d - A)), whether it came from calibration,
// and whether the last Present was depth-tested.
bool OcclusionModel(float& A, float& B, bool& calibrated, bool& lastOccluded);
// Test/diagnostics: how often the W3 depth in use changed, Presents with/without a W3 depth copy,
// and whether occlusion switched itself off after a failure.
void OcclusionStats(uint64_t& switches, uint64_t& framesWith, uint64_t& framesWithout, bool& failed);
} // namespace wc::overlay
