#pragma once
#include <Windows.h>

namespace wc
{
// Optional witchercraft_native.ini next to the DLL:
//   [WitcherCraft]
//   RegisterNatives=1      ; hook native registration and add WCraft_* natives
//   LogChannelFallback=1   ; also listen on Log("WCRAFT:...") from scripts
//   VerboseCalls=20        ; log the first N calls of each native
//   Overlay=1              ; draw Minecraft's HUD over the game (DX12 Present hook); 0 = off
//   OverlayFallbackDelayMs=20000 ; if no swapchain creation was seen by then, find the vtables
//                                ; with a dummy D3D12 device/swapchain instead
//   OverlayPaperWhite=0    ; HDR only: HUD white in nits; 0 = game's HdrPaperWhite (else 200)
//   OverlayColorSpace=auto ; auto | sdr | scrgb | hdr10 (force how the back buffer is encoded)
// Visual sync:
//   OverlayFrameMatch=1    ; draw the Minecraft frame the presented W3 image was rendered with (0 = newest)
//   OverlayLatencyFrames=-1 ; game frames between the script placing the camera and the Present showing
//                          ; it (beyond the measured thread offset): -1 = measure with the image probe
//   OverlayHistory=6       ; Minecraft frames kept on the GPU (2..8)
//   OverlayProbe=1         ; image probe (copies a 48-row band of the picture back each Present)
// Occlusion by Witcher geometry; the Occlusion* keys are re-read every 2 s:
//   OverlayOcclusion=1     ; depth-test Minecraft's pixels against W3's depth buffer (0 = always on top)
//   OcclusionBias=0.15     ; metres (+ 1% of the distance) a W3 surface must be in front to hide MC
//   OcclusionSoft=0.10     ; metres of soft transition (0 = hard edge)
//   OcclusionDepthPick=-1  ; W3 depth candidate index from the log (-1 = automatic)
//   OcclusionReverseZ=-1   ; W3 depth convention: -1 auto, 1 reversed (far = 0), 0 standard (far = 1)
//   OcclusionNear=0.2      ; W3 near plane (m) used until calibrated; OcclusionFar=0 (0 = infinite)
//   OcclusionDepthA= / OcclusionDepthB= ; explicit W3 model z = B / (d - A) (from the calibration log)
//   OcclusionDepthState=0  ; force the W3 depth's resource state at Present (hex D3D12_RESOURCE_STATES)
//   OcclusionKeepNear=1.0  ; Minecraft pixels nearer than this (m: hand, held item) are never hidden
//   OcclusionEyeHeight=1.62 ; ground calibration: eye height above flat ground (m)
//   OcclusionDebug=0       ; 1 = tint hidden MC pixels red instead of hiding, 2 = show W3 depth
//   OverlayEdgeAA=1        ; soften the 1-px staircase on Minecraft's silhouettes (MC has no AA)
//   OcclusionHotkeys=0     ; 1 = Ctrl+Shift+O toggle, +C calibrate, +D debug view, +N next candidate
struct Config
{
    bool registerNatives = true;
    bool logChannelFallback = true;
    int verboseCalls = 20;
    bool overlay = true;
    int overlayFallbackDelayMs = 20000;
    int overlayPaperWhite = 0;
    int overlayColorSpace = -1; // -1 auto, else DXGI_COLOR_SPACE_TYPE (0 SDR, 1 scRGB, 12 HDR10/PQ)
    bool overlayFrameMatch = true;
    int overlayLatencyFrames = -1;
    int overlayHistory = 6;
    bool overlayProbe = true;
    // occlusion
    bool overlayOcclusion = true;
    float occlusionBias = 0.15f;
    float occlusionSoft = 0.10f;
    int occlusionDepthPick = -1;
    int occlusionReverseZ = -1;
    float occlusionNear = 0.2f;
    float occlusionFar = 0.0f;
    bool occlusionModelSet = false; // OcclusionDepthA/B given
    float occlusionDepthA = 0.0f, occlusionDepthB = 0.0f;
    unsigned occlusionDepthState = 0;
    float occlusionEyeHeight = 1.62f;
    float occlusionKeepNear = 1.0f; // MC pixels nearer than this (hand, held item) are never hidden
    int occlusionDebug = 0;
    bool occlusionHotkeys = false;
    bool overlayEdgeAA = true;
    wchar_t iniPath[MAX_PATH * 2] = {};
};

extern Config g_config;
void LoadConfig(HMODULE self);
// Re-reads the Occlusion* keys (and OverlayOcclusion) from the same ini. True if any changed.
bool ReloadOcclusionConfig();
} // namespace wc
