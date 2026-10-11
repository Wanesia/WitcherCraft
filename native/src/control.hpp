#pragma once
// WitcherCraft M3a "Minecraft mode": Minecraft drives the player.
//
//   - Input: a WH_GETMESSAGE hook on the game's window thread reads W3's raw input (WM_INPUT,
//     the game registers raw keyboard + mouse) and, while capturing, forwards keys / buttons /
//     wheel to Minecraft's input ring (SDL scancodes, as SkyCraft's InputBridge expects) and
//     DROPS them for W3 (message -> WM_NULL). W3 keeps Esc, the console, map/journal/inventory
//     keys, F9 and Alt. Mouse motion becomes the look (Minecraft's sensitivity formula, like
//     SkyCraft's Input::ConsumeLook) or, with a Minecraft screen open, Minecraft's cursor.
//   - Look: integrated here (game thread, every frame) and published as SkyState yaw/pitch.
//   - Camera: Minecraft's 20 Hz physics ticks interpolated on the QPC clock exactly like
//     SkyCraft's Game.cpp (tick history + adaptive render delay + walk bob); the script places
//     the W3 camera at that eye every frame (WCraft_ControlFrame / WCraft_ControlLook).
//   - F8 toggles Minecraft mode / Witcher mode (mirror mode from M1/M2).
//
// Threads: OnFrame = game/script thread. HostUpdate/Driven/GetLook = host thread. The input
// hook runs on the game's window thread. Everything shared is atomic or under a mutex.
#include <cstdint>
#include <string>

namespace skycraft::proto
{
struct McState;
}

namespace wc::control
{
constexpr int kToggleVk = 0x77; // VK_F8

// Flags the script passes to OnFrame (WCraft_ControlFrame).
enum FrameFlags : uint32_t
{
    kFrameSuspend = 1u << 0,   // scene / dialogue / vehicle / dead / fading: W3 owns the player now
    kFrameNoCapture = 1u << 1, // a W3 menu (or the console) wants the keyboard and mouse
    kFrameGrounded = 1u << 2,  // M6a: the script keeps hidden Geralt on the ground under Minecraft's
                               // player (informational: status line only)
};

// The toggle (default ON). Any thread.
bool McMode();
void SetMcMode(bool on);
void ToggleMcMode();
// M7: V is held in Minecraft mode (Witcher senses; the script turns W3's focus mode on). Any thread.
bool SensesHeld();
// Minecraft's WitcherCraft options (McState wcFlags, proto::kWc* bits; 0 = an older Minecraft). Any thread.
uint32_t WcFlags();
// The key (DIK) for proto::kWcKey* `which` (interact / Steve-Geralt switch / Witcher senses / Call
// Roach), as Minecraft's WitcherCraft controls set it; proto::kWcKeyNone = unbound. Any thread.
uint32_t KeyDik(uint32_t which);
// A key's name as Windows prints it ("F8", "G", "Num 5"); "none" for kWcKeyNone.
std::string KeyName(uint32_t dik);

struct Frame
{
    bool engaged = false;       // Minecraft drives: script hides Geralt and drives the camera
    bool arrived = false;       // Minecraft's player is where the host put it (no teleport in flight):
                                // only then may the script move Geralt to Minecraft's feet
    bool eyeValid = false;
    double eye[3] = {};         // W3 coords: camera position (MC eye + bob, or MC's F5 camera)
    double feet[3] = {};        // W3 coords: interpolated MC feet
    float headingDeg = 0;       // W3 heading of the camera (0 = +Y, CCW)
    float pitchUpDeg = 0;       // + = looking up (includes the walk-bob dip)
    float rollDeg = 0;          // walk-bob roll
    float fovDeg = 0;           // FOV to give the W3 camera: Minecraft's vertical FOV converted with
                                // FovMode (0 = unknown, leave the camera's FOV alone)
    float mcFovDeg = 0;         // Minecraft's vertical FOV as is
    uint32_t cameraMode = 0;    // Minecraft's F5 mode (0 first person)
    bool fromPose = false;      // eye/look came from the drawn overlay frame's pose (world layer)
    // The PLAYER's look (not the camera's: in F5 "front" mode the camera looks back at Steve).
    // Hidden Geralt faces this way; the interact key aims along it.
    float bodyHeadingDeg = 0;   // W3 heading of Minecraft's player
    float lookPitchUpDeg = 0;   // + = looking up, no walk bob
    float eyeHeight = 1.62f;    // Minecraft's eye height above the feet (sneaking lowers it)
};

// Camera calibration (worldlayer.md "FOV contract"). Ini [WitcherCraft] in witchercraft_native.ini:
//   CameraFovMode=0  ; how W3 takes a FOV: 0 vertical, 1 horizontal at the real aspect, 2 horizontal at 4:3
//   CameraPose=1     ; 1 = place the W3 camera with the pose of the overlay frame being drawn (world
//                    ;     layer lock) when available, 0 = always SkyCraft's own tick interpolation
// Console (script): wcfov(n), wccamsrc(n). Any thread.
void SetFovMode(int mode);
int FovMode();
void SetUsePose(bool on);
bool UsePose();
// Frame pacing: ini McPace=1 (Minecraft renders once per W3 game frame, with that
// frame's look; 0 = SkyCraft's ~60 Hz SkyState pacing), McMaxFps=0 (cap, 0 = none). Any thread.
void SetMcPace(int pace, int maxFps);
// The overlay's pose getter (registered by natives_control.cpp; the offline tools have none).
struct OverlayPose
{
    uint64_t frameId = 0, takenMs = 0;
    double eye[3] = {};
    float yaw = 0, pitch = 0, fovDeg = 0, rollDeg = 0;
};
using PoseProvider = bool (*)(OverlayPose& out);
void SetPoseProvider(PoseProvider fn);

// Script thread, once per rendered frame. camHeading / camPitchUp = the current W3 camera look
// (adopted as the look when Minecraft mode engages).
Frame OnFrame(uint32_t flags, float camHeadingDeg, float camPitchUpDeg);
Frame LastFrame();

// Host thread, every iteration (before SkyState is written). inGameReady = script ticks fresh,
// in game, not loading. Installs the input hook, tracks Minecraft screens (cursor), releases
// Minecraft's keys when capture stops.
void HostUpdate(const skycraft::proto::McState* mc, bool mcAlive, bool inGameReady, uint32_t viewportW, uint32_t viewportH);
// Host thread, after its teleport logic: Minecraft's player has acknowledged the last teleport,
// none is pending or held. Minecraft mode only engages once this is true (SkyCraft's "puppet").
void SetArrived(bool arrived);
// Host thread: Minecraft drives the player (host stops mirror-following, publishes GetLook).
bool Driven();
// MC degrees (yaw 0 = +Z south, pitch + = down). Any thread.
void GetLook(float& yaw, float& pitch);
void SetLook(float yaw, float pitch);
// True while the hook is taking input away from W3 (Minecraft mode, fresh frames, no W3 menu).
bool Capturing();

// For the overlay (any thread): Minecraft's cursor in overlay pixels while a Minecraft screen is
// open and we capture the mouse (SkyCraft's Overlay.cpp draws it; Minecraft doesn't). False = hide.
bool Cursor(int& x, int& y);

// ---- W3 interact key ----------------------------------------------------
// In Minecraft mode the interact key (ini InteractKey, DIK scan code in decimal, default 34 = G)
// never reaches Minecraft. Through the GetRawInputData hook it is handed to The Witcher AS its
// own interaction key (ini InteractW3Key, default 18 = E, W3's Interaction/Talk/Container/Open/
// MountHorse binding), so the engine's active interaction runs exactly as if E was pressed in
// Witcher mode. Every press is also counted for the script, which falls back to its own target
// search when the engine had no active interaction (or the remap was impossible).
struct InteractPoll
{
    uint32_t presses = 0;   // presses since the last poll
    uint32_t givenToW3 = 0; // of those, how many reached W3 as its interaction key
    bool held = false;      // the key is down right now
    uint32_t dik = 0;       // the interact key (DIK)
};
InteractPoll ConsumeInteract(); // script thread (WCraft_InteractPoll)

// Offline tests: run a RAWINPUT through exactly the path W3's GetRawInputData calls take (the
// event is modified in place: blanked, remapped, or left alone). rawInput is a RAWINPUT*.
void TestRawInput(void* rawInput, void* handle);
struct InputStats
{
    uint64_t keysSent = 0, keysPassed = 0, interactPresses = 0, interactRemapped = 0;
    uint64_t mcMenuOpens = 0;  // kInOpenMenu sent (Esc / O)
    uint64_t w3MenuSwitch = 0; // panel keys that switched to Witcher mode (N, meditation)
    uint64_t w3Panels = 0;     // panel keys that opened a Witcher panel over Minecraft mode (M/J/I/K)
    uint64_t panelReTicks = 0; // host ticks sent for the script while a panel paused it
};
InputStats Inputs();

// ---- M6a mode identity ---------------------------------------------------
// Minecraft mode = Minecraft's identity: Esc opens Minecraft's pause menu (kInOpenMenu, as O
// does), and The Witcher's panel keys open those panels as Steve (M map, J journal, I inventory,
// K character: see PanelBegin) or, for N meditation, switch to Witcher (Geralt) mode first; the
// script starts the meditation once Geralt is shown and standing. Ini [WitcherCraft] McIdentity=1 (0 = the M3 key map: Esc and the panel keys are
// The Witcher's in Minecraft mode).
bool IdentityKeys();
// Script thread (WCraft_ModeRequest): the panel key (DIK) whose panel should open now (N: it also
// switched to Witcher mode; M/J/I/K: Minecraft mode stays, see PanelBegin); 0 = none. Consumed.
uint32_t ConsumeW3MenuRequest();
// The Minecraft overlay (hand, hotbar, crosshair, world layer) is drawn only in Minecraft mode
// (and not while a scene/dialogue/vehicle suspends it), or in Witcher mode while F7 build mode
// is on. Witcher mode is pure Witcher. Any thread (overlay_dx12 Present hook).
bool OverlayWanted();
// Minecraft drives and the script keeps Geralt on the ground under Steve (kFrameGrounded).
bool GeraltGrounded();

// ---- The Witcher's panels as Steve ----------------------------------------------------------
// M map / J journal / I inventory / K character in Minecraft mode (option kWcPanelKeys) open that
// Witcher panel OVER Minecraft mode, without becoming Geralt: the key press starts the panel state
// (input to The Witcher, Minecraft paused through WcPace kPacePanel, no overlay), the script opens
// the panel and reports Geralt's (hidden, grounded) position; while the menu pauses the script, the
// host thread ticks the host for it with that position (no stale-tick "load", so Steve is never
// re-placed). The script ends it when the menu is gone. N (meditation) still switches to Geralt.
// Script thread (WCraft_Panel): the panel was opened; Geralt stands at (x, y, z) (W3 coords).
void PanelBegin(float x, float y, float z);
// Script thread: The Witcher's panel is closed (or could not open): Minecraft mode resumes.
void PanelEnd(const char* why);
// 1 = panel state on, 2 = the script was paused meanwhile (a Witcher menu was up), 4 = opened (PanelBegin).
uint32_t PanelState();

// Offline tests (fake_host): raw mouse counts as if they came from WM_INPUT.
void InjectMouse(int dx, int dy);
// Offline tests: count frames (OnFrame) whose eye moved by less than 1 mm while walking etc.
struct MotionStats
{
    uint64_t frames = 0, lateFrames = 0, ticks = 0;
    double renderDelayMs = 0;
};
MotionStats Motion();

std::string StatusSuffix();
} // namespace wc::control
