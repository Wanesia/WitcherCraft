// WitcherCraft M3a: Minecraft drives the player. See control.hpp.
#include "control.hpp"

#include "buildmode.hpp"
#include "camsync.hpp"
#include "log.hpp"
#include "skycraft_host.hpp"
#include "skycraft_protocol.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <type_traits>

namespace wc::control
{
namespace proto = skycraft::proto;

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr uint64_t kCaptureFreshMs = 150; // no script frame for this long (W3 paused / menu): give input back
constexpr uint64_t kDrivenFreshMs = 750;  // like the host's stale-tick limit

// ---- user32, resolved at run time (no new static imports; see buildmode.cpp) ----------------
struct User32
{
    UINT(WINAPI* GetRawInputData)(HRAWINPUT, UINT, LPVOID, PUINT, UINT) = nullptr;
    HHOOK(WINAPI* SetWindowsHookExW)(int, HOOKPROC, HINSTANCE, DWORD) = nullptr;
    LRESULT(WINAPI* CallNextHookEx)(HHOOK, int, WPARAM, LPARAM) = nullptr;
    HWND(WINAPI* GetForegroundWindow)() = nullptr;
    DWORD(WINAPI* GetWindowThreadProcessId)(HWND, LPDWORD) = nullptr;
    BOOL(WINAPI* TranslateMessage)(const MSG*) = nullptr;
    BOOL(WINAPI* UnhookWindowsHookEx)(HHOOK) = nullptr;
    UINT(WINAPI* MapVirtualKeyW)(UINT, UINT) = nullptr;
    int(WINAPI* ShowCursor)(BOOL) = nullptr;
    HCURSOR(WINAPI* SetCursor)(HCURSOR) = nullptr;
    int(WINAPI* GetKeyNameTextW)(LONG, LPWSTR, int) = nullptr;
    SHORT(WINAPI* GetAsyncKeyState)(int) = nullptr;
    bool ok = false;
};
const User32& U()
{
    static const User32 u = [] {
        User32 r;
        HMODULE m = GetModuleHandleW(L"user32.dll");
        if (!m)
            m = LoadLibraryW(L"user32.dll");
        if (!m)
            return r;
        auto get = [m](auto& fn, const char* name) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(m, name)); };
        get(r.GetRawInputData, "GetRawInputData");
        get(r.SetWindowsHookExW, "SetWindowsHookExW");
        get(r.CallNextHookEx, "CallNextHookEx");
        get(r.GetForegroundWindow, "GetForegroundWindow");
        get(r.GetWindowThreadProcessId, "GetWindowThreadProcessId");
        get(r.TranslateMessage, "TranslateMessage");
        get(r.UnhookWindowsHookEx, "UnhookWindowsHookEx");
        get(r.MapVirtualKeyW, "MapVirtualKeyW");
        get(r.ShowCursor, "ShowCursor");
        get(r.SetCursor, "SetCursor");
        get(r.GetKeyNameTextW, "GetKeyNameTextW");
        get(r.GetAsyncKeyState, "GetAsyncKeyState");
        r.ok = r.GetRawInputData && r.SetWindowsHookExW && r.CallNextHookEx && r.GetForegroundWindow && r.GetWindowThreadProcessId;
        return r;
    }();
    return u;
}

// DirectInput / set-1 scan code (E0-prefixed codes | 0x80) -> SDL scancode. Same table as
// SkyCraft's skse/src/Input.cpp (MIT).
constexpr auto kDikToSdl = [] {
    std::array<uint16_t, 256> t{};
    t[0x01] = 41; // Esc
    for (int i = 0; i < 9; ++i)
        t[0x02 + i] = static_cast<uint16_t>(30 + i); // 1-9
    t[0x0B] = 39;                                     // 0
    t[0x0C] = 45, t[0x0D] = 46, t[0x0E] = 42, t[0x0F] = 43;
    t[0x10] = 20, t[0x11] = 26, t[0x12] = 8, t[0x13] = 21, t[0x14] = 23;
    t[0x15] = 28, t[0x16] = 24, t[0x17] = 12, t[0x18] = 18, t[0x19] = 19;
    t[0x1A] = 47, t[0x1B] = 48, t[0x1C] = 40, t[0x1D] = 224;
    t[0x1E] = 4, t[0x1F] = 22, t[0x20] = 7, t[0x21] = 9, t[0x22] = 10;
    t[0x23] = 11, t[0x24] = 13, t[0x25] = 14, t[0x26] = 15;
    t[0x27] = 51, t[0x28] = 52, t[0x29] = 53, t[0x2A] = 225, t[0x2B] = 49;
    t[0x2C] = 29, t[0x2D] = 27, t[0x2E] = 6, t[0x2F] = 25, t[0x30] = 5;
    t[0x31] = 17, t[0x32] = 16, t[0x33] = 54, t[0x34] = 55, t[0x35] = 56;
    t[0x36] = 229, t[0x37] = 85, t[0x38] = 226, t[0x39] = 44, t[0x3A] = 57;
    for (int i = 0; i < 10; ++i)
        t[0x3B + i] = static_cast<uint16_t>(58 + i); // F1-F10
    t[0x45] = 83, t[0x46] = 71;
    t[0x47] = 95, t[0x48] = 96, t[0x49] = 97, t[0x4A] = 86;
    t[0x4B] = 92, t[0x4C] = 93, t[0x4D] = 94, t[0x4E] = 87;
    t[0x4F] = 89, t[0x50] = 90, t[0x51] = 91, t[0x52] = 98, t[0x53] = 99;
    t[0x56] = 100, t[0x57] = 68, t[0x58] = 69;
    t[0x9C] = 88, t[0x9D] = 228, t[0xB5] = 84, t[0xB7] = 70, t[0xB8] = 230;
    t[0xC5] = 72, t[0xC7] = 74, t[0xC8] = 82, t[0xC9] = 75, t[0xCB] = 80;
    t[0xCD] = 79, t[0xCF] = 77, t[0xD0] = 81, t[0xD1] = 78, t[0xD2] = 73;
    t[0xD3] = 76, t[0xDB] = 227, t[0xDC] = 231, t[0xDD] = 101;
    return t;
}();

constexpr uint32_t kDikEsc = 0x01, kDikTilde = 0x29, kDikI = 0x17, kDikJ = 0x24, kDikK = 0x25, kDikM = 0x32, kDikN = 0x31,
                   kDikO = 0x18, kDikF8 = 0x42, kDikF9 = 0x43, kDikF10 = 0x44, kDikF11 = 0x57, kDikF12 = 0x58, kDikLAlt = 0x38,
                   kDikRAlt = 0xB8, kDikLWin = 0xDB, kDikRWin = 0xDC, kDikPrtSc = 0xB7, kDikX = 0x2D, kDikV = 0x2F;

std::atomic<int> g_identity{-1}; // ini McIdentity (M6a); -1 = not loaded yet (= on)
std::atomic<uint32_t> g_wcFlags{0}; // McState wcFlags (Minecraft's WitcherCraft options; 0 = an older Minecraft)
std::atomic<uint32_t> g_wcKeys{0};  // McState wcKeys (one DIK per proto::kWcKey* byte)
std::atomic<uint32_t> g_modeDik{kDikF8}; // the Steve / Geralt switch (Minecraft's WitcherCraft controls; F8)

bool IsFunctionKey(uint32_t dik)
{
    return (dik >= 0x3B && dik <= 0x44) || dik == kDikF11 || dik == kDikF12; // F1-F10, F11, F12
}

// M/J/I/K/N switch to Witcher mode first (the WitcherCraft option; always with an older Minecraft).
bool PanelKeysSwitch()
{
    const uint32_t f = g_wcFlags.load(std::memory_order_relaxed);
    return !(f & proto::kWcValid) || (f & proto::kWcPanelKeys);
}

bool IdentityNow()
{
    return g_identity.load(std::memory_order_relaxed) != 0;
}

// The Witcher's panel keys: in Minecraft mode (McIdentity=1) they switch to Witcher mode first.
bool IsW3PanelKey(uint32_t dik)
{
    return dik == kDikM || dik == kDikJ || dik == kDikI || dik == kDikK || dik == kDikN;
}

// Keys The Witcher keeps while Minecraft drives (everything else is Minecraft's). With a
// Minecraft screen open (inventory, chat) every key is Minecraft's except the system ones,
// so typing works and Esc closes the screen. M6a (McIdentity=1): Esc is Minecraft's (its pause
// menu) and the panel keys switch to Witcher mode (OnRawKeyboard); only the console and F9 stay.
bool IsW3Key(uint32_t dik, bool mcScreen)
{
    if (dik == kDikF8 || dik == kDikLAlt || dik == kDikRAlt || dik == kDikLWin || dik == kDikRWin || dik == kDikPrtSc || dik == kDikF12 ||
        dik == kDikF11 || dik == kDikF10)
        return true;
    if (mcScreen)
        return false;
    // X calls Roach: with a new Minecraft (WitcherCraft options present) Steve's own Minecraft
    // Roach (its "Call Roach" key binding); with an older one The Witcher's (SpawnHorse).
    if (dik == kDikTilde || dik == kDikF9 || (dik == kDikX && !(g_wcFlags.load(std::memory_order_relaxed) & proto::kWcValid)))
        return true;
    if (IdentityNow())
        return false;
    return dik == kDikEsc || IsW3PanelKey(dik);
}

// ---- shared state ----------------------------------------------------------------------------
std::atomic<bool> g_mcMode{true};
std::atomic<bool> g_frameEngaged{false};
std::atomic<uint32_t> g_frameFlags{0};
std::atomic<uint64_t> g_lastFrameMs{0};
std::atomic<bool> g_hostMcOk{false};   // MC linked and its player in the world
std::atomic<bool> g_hostReady{false};  // script ticking, in game, not loading
std::atomic<bool> g_driven{false};
std::atomic<bool> g_hostArrived{false}; // MC acknowledged the last teleport; none pending
std::atomic<bool> g_screenOpen{false};
std::atomic<float> g_sensitivity{0.5f};
std::atomic<float> g_yaw{0}, g_pitch{0};
std::atomic<int> g_dx{0}, g_dy{0};
std::atomic<int> g_fovMode{-1};      // -1 = not loaded from the ini yet
std::atomic<int> g_usePose{-1};
std::atomic<PoseProvider> g_poseProvider{nullptr};
std::atomic<uint32_t> g_vpW{0}, g_vpH{0};
std::atomic<uint64_t> g_poseFrames{0};
std::atomic<int> g_mcPace{1};      // ini McPace: 1 = Minecraft renders once per W3 game frame (sync.md)
std::atomic<int> g_mcMaxFps{0};    // ini McMaxFps: cap for that (0 = none)
std::atomic<uint32_t> g_interactDik{0x22};   // ini InteractKey (DIK, decimal): G, like SkyCraft's activate key
std::atomic<uint32_t> g_w3InteractDik{0x12}; // ini InteractW3Key: what W3 gets instead (its Interaction key, E)
std::atomic<uint32_t> g_interactPresses{0}, g_interactGiven{0};
std::atomic<bool> g_interactHeld{false};
std::atomic<uint64_t> g_interactTotal{0}, g_interactRemapTotal{0};
std::atomic<uint32_t> g_w3MenuReq{0};                     // M6a: panel key whose panel the script should open
std::atomic<bool> g_sensesHeld{false};                     // M7: V held (Witcher senses), read by the script
bool g_sensesDown = false;                                 // input thread: V's press was taken
std::atomic<uint64_t> g_mcMenuOpens{0}, g_w3MenuSwitch{0}; // M6a counters

// ---- The Witcher's panels as Steve -----------------------------------------------------------
// M/J/I/K open the Witcher's map / journal / inventory / character panel OVER Minecraft mode:
// Minecraft mode stays on, Geralt stays hidden on the ground under Steve, Minecraft pauses itself
// (WcPace kPacePanel -> WcPanelPause.java: Gui.isPausing) so Steve keeps his exact place and speed
// (also mid-air), and while The Witcher's menu pauses the script we keep the host's script ticks
// alive (host::Tick with Geralt's last position, "no follow") so the host never sees "stale ticks"
// = loading, which used to re-place Steve onto grounded Geralt. The script ends it when the menu
// is gone; nothing switches modes. N (meditation) still switches to Geralt (he sits down while
// time passes) and the script brings Steve back when the meditation ends.
constexpr uint32_t kPacePanel = proto::kPacePanel; // WcPace flag: Minecraft pauses
constexpr uint64_t kPanelReTickMs = 100; // script quiet this long during a panel = The Witcher paused it: tick for it
std::atomic<bool> g_panelActive{false};
std::atomic<bool> g_panelConfirmed{false}; // the script has opened the panel (PanelBegin)
std::atomic<bool> g_panelSawPause{false};  // the script was paused meanwhile (a Witcher menu was up)
std::atomic<uint64_t> g_panelSinceMs{0}, g_panelNoMenuSinceMs{0};
std::atomic<uint32_t> g_panelKey{0};
std::mutex g_panelMutex; // g_panelPos
float g_panelPos[3] = {};
std::atomic<uint64_t> g_panelOpens{0}, g_panelReTicks{0};
bool g_panelKeyHeld[256] = {}; // input thread: pressed while a panel was open
std::mutex g_paceMutex;        // PublishPace: the script thread, and the host thread while the script is paused

void EndPanel(const char* why)
{
    if (g_panelActive.exchange(false))
    {
        g_panelConfirmed = false;
        WLOG("control: Witcher panel (DIK 0x%02X) closed after %.1f s (%s): Minecraft mode resumes, Steve where he was",
             g_panelKey.load(), (GetTickCount64() - g_panelSinceMs.load()) / 1000.0, why);
    }
}

void LoadIni()
{
    if (g_fovMode.load() >= 0 && g_usePose.load() >= 0 && g_identity.load() >= 0)
        return;
    wchar_t path[MAX_PATH * 2]{};
    HMODULE self = nullptr;
    int fov = 0, pose = 1, pace = 1, maxFps = 0, iKey = 0x22, iW3Key = 0x12, identity = 1;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&LoadIni),
                           &self) &&
        GetModuleFileNameW(self, path, MAX_PATH * 2))
    {
        if (wchar_t* slash = wcsrchr(path, L'\\'))
        {
            wcscpy_s(slash + 1, MAX_PATH * 2 - (slash + 1 - path), L"witchercraft_native.ini");
            fov = static_cast<int>(GetPrivateProfileIntW(L"WitcherCraft", L"CameraFovMode", 0, path));
            pose = static_cast<int>(GetPrivateProfileIntW(L"WitcherCraft", L"CameraPose", 1, path));
            pace = static_cast<int>(GetPrivateProfileIntW(L"WitcherCraft", L"McPace", 1, path));
            maxFps = static_cast<int>(GetPrivateProfileIntW(L"WitcherCraft", L"McMaxFps", 0, path));
            iKey = static_cast<int>(GetPrivateProfileIntW(L"WitcherCraft", L"InteractKey", 0x22, path));
            iW3Key = static_cast<int>(GetPrivateProfileIntW(L"WitcherCraft", L"InteractW3Key", 0x12, path));
            identity = static_cast<int>(GetPrivateProfileIntW(L"WitcherCraft", L"McIdentity", 1, path));
        }
    }
    if (g_fovMode.load() < 0)
    {
        g_mcPace = pace ? 1 : 0;
        g_mcMaxFps = std::clamp(maxFps, 0, 1000);
        WLOG("control: McPace=%d (1 = Minecraft renders one frame per Witcher game frame) McMaxFps=%d", g_mcPace.load(), g_mcMaxFps.load());
        if (iKey > 0 && iKey < 256 && iKey != 0x42) // never F8
            g_interactDik = static_cast<uint32_t>(iKey);
        if (iW3Key > 0 && iW3Key < 256)
            g_w3InteractDik = static_cast<uint32_t>(iW3Key);
        WLOG("control: InteractKey=%u (DIK; 34 = G) -> The Witcher's interaction key InteractW3Key=%u (18 = E)", g_interactDik.load(),
             g_w3InteractDik.load());
    }
    int expected = -1;
    if (g_identity.compare_exchange_strong(expected, identity ? 1 : 0))
        WLOG("control: McIdentity=%d (1 = in Minecraft mode Esc opens Minecraft's menu and M/J/I/K/N switch to Witcher mode first)",
             g_identity.load());
    expected = -1;
    if (g_fovMode.compare_exchange_strong(expected, std::clamp(fov, 0, 2)))
        WLOG("control: CameraFovMode=%d (0 vertical, 1 horizontal at the real aspect, 2 horizontal at 4:3)", g_fovMode.load());
    expected = -1;
    if (g_usePose.compare_exchange_strong(expected, pose ? 1 : 0))
        WLOG("control: CameraPose=%d (1 = lock the camera to the drawn overlay frame's pose)", g_usePose.load());
}

// Minecraft's vertical FOV -> what the W3 camera takes (FovMode).
float ConvertFov(float vDeg)
{
    if (vDeg < 10.0f)
        return 0.0f;
    const int mode = g_fovMode.load();
    if (mode == 0)
        return vDeg;
    const uint32_t w = g_vpW.load(), h = g_vpH.load();
    const double aspect = mode == 2 ? 4.0 / 3.0 : (w && h ? static_cast<double>(w) / h : 16.0 / 9.0);
    return static_cast<float>(2.0 * std::atan(std::tan(vDeg * 0.5 * kDeg) * aspect) / kDeg);
}
std::atomic<uint64_t> g_keysSent{0}, g_mouseMsgs{0}, g_keysPassed{0};

void Queue(uint16_t type, uint16_t code, int32_t a, int32_t b = 0)
{
    buildmode::QueueEvent({type, code, a, b, 0});
}

// ---- input hook (game window thread only) ------------------------------------------------
HHOOK g_hook = nullptr;
std::atomic<bool> g_rawHooked{false};
bool g_rawHookTried = false;
bool g_ownedKey[256] = {};
bool g_ownedButton[6] = {};
bool g_anyOwned = false;
bool g_f8Down = false;
wchar_t g_highSurrogate = 0;

bool CaptureNow()
{
    return g_mcMode.load(std::memory_order_relaxed) && g_frameEngaged.load(std::memory_order_relaxed) &&
           !g_panelActive.load(std::memory_order_relaxed) &&
           GetTickCount64() - g_lastFrameMs.load(std::memory_order_relaxed) < kCaptureFreshMs &&
           !(g_frameFlags.load(std::memory_order_relaxed) & kFrameNoCapture);
}

// Set when an event reached through the GetRawInputData hook (no MSG) should be hidden from W3.
thread_local bool t_dropRaw = false;
// Set when the event is the interact key and W3 should see it as its own interaction key.
thread_local bool t_remapRaw = false;
bool g_interactDown = false;     // input thread: the interact key's press was taken
bool g_swallowUp[256] = {};      // input thread (M6a): this key's press was taken; hide its repeats and release from both games
bool g_interactRemapped = false; // ... and handed to W3 as its interaction key

void Drop(MSG* m)
{
    if (!m)
    {
        t_dropRaw = true;
        return;
    }
    m->message = WM_NULL;
    m->wParam = 0;
    m->lParam = 0;
}

void OnRawKeyboard(const RAWKEYBOARD& kb, bool capture, bool screen, MSG* m)
{
    if (kb.Flags & RI_KEY_E1)
        return; // Pause/Break
    const uint32_t dik = (kb.MakeCode & 0x7F) | ((kb.Flags & RI_KEY_E0) ? 0x80u : 0u);
    if (dik == 0xAA || dik == 0xB6 || dik == 0)
        return; // the fake shifts around E0 keys
    const bool down = !(kb.Flags & RI_KEY_BREAK);
    // The Steve / Geralt switch (F8 unless remapped in Minecraft's WitcherCraft controls). A typing
    // key while a Minecraft screen is open (chat, a sign) stays a typing key.
    if (dik == g_modeDik.load(std::memory_order_relaxed) && (g_f8Down || !screen || IsFunctionKey(dik)))
    {
        if (down && !g_f8Down)
            ToggleMcMode();
        g_f8Down = down;
        if (dik != kDikF8)
            Drop(m); // neither game gets another key; W3 still sees F8 (unbound there), as before
        return;
    }
    // M7: hold V for Witcher senses in Minecraft mode (W3's own key, the right mouse button, is
    // Minecraft's use/place). Neither game gets V; the script turns W3's focus mode on while held
    // (WCraft_SensesHeld). Its release follows its press even if capture stopped meanwhile.
    // (Only with an older Minecraft: a new one has its own remappable "Witcher senses" key binding
    // and reports it in McState wcFlags, so V then reaches Minecraft like any key.)
    if (dik == kDikV && !(g_wcFlags.load(std::memory_order_relaxed) & proto::kWcValid) && (g_sensesDown || (capture && !screen)))
    {
        if (down != g_sensesDown)
            WLOG("control: Witcher senses key %s", down ? "held" : "released");
        g_sensesDown = down;
        g_sensesHeld.store(down);
        Drop(m);
        return;
    }
    // The W3 interact key: never Minecraft's while capturing; W3 gets it as
    // its interaction key. Its release follows its press (also when capture stopped meanwhile,
    // e.g. the dialogue it opened), so W3 never sees the interaction key stuck down.
    if (dik == g_interactDik.load(std::memory_order_relaxed))
    {
        if (down && (g_interactDown || (capture && !screen)))
        {
            if (!g_interactDown)
            {
                g_interactDown = true;
                g_interactRemapped = (m == nullptr); // the message-hook fallback can't rewrite W3's raw input
                g_interactPresses.fetch_add(1);
                ++g_interactTotal;
                if (g_interactRemapped)
                {
                    g_interactGiven.fetch_add(1);
                    ++g_interactRemapTotal;
                }
                g_interactHeld = true;
            }
            if (m || !g_interactRemapped)
                Drop(m);
            else
                t_remapRaw = true;
            return;
        }
        if (!down && g_interactDown)
        {
            g_interactDown = false;
            g_interactHeld = false;
            if (m || !g_interactRemapped)
                Drop(m);
            else
                t_remapRaw = true;
            return;
        }
        // Witcher mode / a Minecraft screen (typing): an ordinary key.
    }
    // M6a: a key whose press opened Minecraft's menu or switched modes: its auto-repeats and its
    // release reach neither game (also after capture stopped because of that very press).
    if (g_swallowUp[dik])
    {
        if (!down)
            g_swallowUp[dik] = false;
        Drop(m);
        return;
    }
    // A panel key or Esc pressed inside an open Witcher panel (to close it) whose release only
    // arrives once the panel is gone: W3 opens its panels on the RELEASE, so that release would
    // reopen one over Minecraft mode without us. It reaches neither game.
    if (IsW3PanelKey(dik) || dik == kDikEsc)
    {
        if (down && g_panelActive.load(std::memory_order_relaxed))
            g_panelKeyHeld[dik] = true;
        else if (!down && g_panelKeyHeld[dik])
        {
            g_panelKeyHeld[dik] = false;
            if (!g_panelActive.load(std::memory_order_relaxed))
            {
                Drop(m);
                return;
            }
        }
    }
    if (!capture)
        return;
    if (!down)
    {
        if (g_ownedKey[dik])
        {
            g_ownedKey[dik] = false;
            if (const uint16_t sdl = kDikToSdl[dik])
                Queue(proto::kInKey, sdl, 0);
            Drop(m);
        }
        return;
    }
    if (IsW3Key(dik, screen))
    {
        ++g_keysPassed;
        return;
    }
    if (!screen && IdentityNow())
    {
        if (dik == kDikEsc)
        {
            // M6a: Minecraft mode has Minecraft's identity: Esc is its pause/options menu.
            Queue(proto::kInReleaseAll, 0, 0);
            Queue(proto::kInOpenMenu, 0, 0);
            ++g_mcMenuOpens;
            g_swallowUp[dik] = true;
            Drop(m);
            return;
        }
        if (IsW3PanelKey(dik) && PanelKeysSwitch() && dik != kDikN)
        {
            // The Witcher's map / journal / inventory / character panel as Steve: Minecraft mode stays
            // on; input goes to The Witcher and Minecraft pauses from now on (g_panelActive); the
            // script opens the panel (WCraft_ModeRequest) and ends the panel state when it closes.
            g_swallowUp[dik] = true;
            g_w3MenuReq.store(dik);
            g_panelKey.store(dik);
            g_panelSawPause = false;
            g_panelConfirmed = false;
            g_panelNoMenuSinceMs = 0;
            g_panelSinceMs = GetTickCount64();
            g_panelActive = true;
            ++g_panelOpens;
            Drop(m);
            WLOG("control: Witcher panel key (DIK 0x%02X) as Steve: opening that panel over Minecraft mode (Minecraft paused, Steve stays)",
                 dik);
            return;
        }
        if (IsW3PanelKey(dik) && PanelKeysSwitch())
        {
            // N, meditation: Geralt sits down while time passes, so switch to Witcher mode first
            // (Geralt shown, standing, his HUD back); the script then starts the meditation and
            // returns to Minecraft mode when it ends.
            g_swallowUp[dik] = true;
            g_w3MenuReq.store(dik);
            ++g_w3MenuSwitch;
            Drop(m);
            WLOG("control: Witcher panel key (DIK 0x%02X) in Minecraft mode: switching to Witcher mode first", dik);
            SetMcMode(false);
            return;
        }
    }
    if (dik == kDikO && !screen)
    {
        // Minecraft's pause/options menu (Esc stays The Witcher's), like SkyCraft.
        if (!g_ownedKey[dik])
        {
            Queue(proto::kInReleaseAll, 0, 0);
            Queue(proto::kInOpenMenu, 0, 0);
        }
        g_ownedKey[dik] = true;
        g_anyOwned = true;
        Drop(m);
        return;
    }
    if (const uint16_t sdl = kDikToSdl[dik])
    {
        Queue(proto::kInKey, sdl, 1); // repeats are fine: InputBridge treats down-while-down as repeat
        g_ownedKey[dik] = true;
        g_anyOwned = true;
        ++g_keysSent;
    }
    Drop(m);
}

// Capture just came back (after a W3 menu, a hold gate, a dialogue): kInReleaseAll let go of every
// key, and Windows only auto-repeats the LAST key pressed, so W held under Shift would stay
// released and Steve would stand still until W is pressed again. Re-press the keys that are
// physically down now (not the host's own keys). Input thread.
bool g_resumeCapture = false;
std::atomic<uint64_t> g_keysResumed{0};
void ResumeHeldKeys(bool capture, bool screen)
{
    const bool now = capture && !screen;
    if (now == g_resumeCapture)
        return;
    g_resumeCapture = now;
    if (!now || !U().GetAsyncKeyState || !U().MapVirtualKeyW)
        return;
    int n = 0;
    for (uint32_t dik = 1; dik < 256; ++dik)
    {
        const uint16_t sdl = kDikToSdl[dik];
        if (!sdl || g_ownedKey[dik] || dik == g_modeDik.load(std::memory_order_relaxed) || dik == g_interactDik.load(std::memory_order_relaxed) ||
            dik == kDikEsc || dik == kDikO || IsW3PanelKey(dik) || IsW3Key(dik, false))
            continue;
        const UINT vk = U().MapVirtualKeyW((dik & 0x80) ? (0xE000u | (dik & 0x7F)) : dik, 3 /* MAPVK_VSC_TO_VK_EX */);
        if (vk && (U().GetAsyncKeyState(static_cast<int>(vk)) & 0x8000))
        {
            Queue(proto::kInKey, sdl, 1);
            g_ownedKey[dik] = true;
            g_anyOwned = true;
            ++n;
        }
    }
    if (n)
    {
        g_keysResumed += static_cast<uint64_t>(n);
        WLOG("control: capture back: %d held key(s) pressed again for Minecraft", n);
    }
}

void OnRawMouse(const RAWMOUSE& ms, MSG* m)
{
    ++g_mouseMsgs;
    if (!(ms.usFlags & MOUSE_MOVE_ABSOLUTE))
    {
        if (ms.lLastX)
            g_dx += ms.lLastX;
        if (ms.lLastY)
            g_dy += ms.lLastY;
    }
    static constexpr struct
    {
        USHORT down, up;
        uint16_t sdl;
        int slot;
    } kButtons[] = {
        {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, 1, 1},
        {RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, 3, 3},
        {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, 2, 2},
        {RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, 4, 4},
        {RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, 5, 5},
    };
    const USHORT f = ms.usButtonFlags;
    for (const auto& b : kButtons)
    {
        if (f & b.down)
        {
            Queue(proto::kInMouseButton, b.sdl, 1);
            g_ownedButton[b.slot] = true;
            g_anyOwned = true;
        }
        if ((f & b.up) && g_ownedButton[b.slot])
        {
            Queue(proto::kInMouseButton, b.sdl, 0);
            g_ownedButton[b.slot] = false;
        }
    }
    if (f & RI_MOUSE_WHEEL)
    {
        const int delta = static_cast<SHORT>(ms.usButtonData);
        if (delta)
            Queue(proto::kInScroll, 0, delta);
    }
    Drop(m);
}

// Input thread: the Windows cursor is hidden by us (one ShowCursor(FALSE) outstanding).
bool g_osCursorHidden = false;

// In Minecraft mode W3 never sees the mouse (its messages are dropped), so it never hides the
// Windows cursor by itself: hide it while Minecraft has the mouse. With a Minecraft screen open,
// Minecraft draws its own cursor into the overlay frame at exactly its mouse position
// (WcCursor.java), so a Windows cursor (wherever Windows thinks it is) would only be a second,
// wrong one. Input thread (it owns the game window).
void UpdateOsCursor(bool capture, bool /*screen*/)
{
    const bool hide = capture;
    if (hide != g_osCursorHidden && U().ShowCursor)
    {
        U().ShowCursor(hide ? FALSE : TRUE);
        g_osCursorHidden = hide;
    }
    if (hide && U().SetCursor)
        U().SetCursor(nullptr); // the window class cursor comes back with each WM_SETCURSOR
}

void HandleMessage(MSG* m)
{
    const bool capture = CaptureNow();
    const bool screen = g_screenOpen.load(std::memory_order_relaxed);
    UpdateOsCursor(capture, screen);
    if (!capture && g_anyOwned)
    {
        // Capture just stopped (menu, toggle, scene): the host sends kInReleaseAll; forget ours.
        std::fill(std::begin(g_ownedKey), std::end(g_ownedKey), false);
        std::fill(std::begin(g_ownedButton), std::end(g_ownedButton), false);
        g_anyOwned = false;
    }
    ResumeHeldKeys(capture, screen);
    switch (m->message)
    {
    case WM_INPUT: {
        // With the GetRawInputData import hooked, raw input is handled there (once, whatever thread
        // W3 reads it on); handling it here too would forward every event twice.
        if (g_rawHooked.load(std::memory_order_relaxed) || !U().GetRawInputData)
            return;
        RAWINPUT raw;
        UINT size = sizeof(raw);
        if (U().GetRawInputData(reinterpret_cast<HRAWINPUT>(m->lParam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
            return;
        if (raw.header.dwType == RIM_TYPEKEYBOARD)
            OnRawKeyboard(raw.data.keyboard, capture, screen, m);
        else if (raw.header.dwType == RIM_TYPEMOUSE && capture)
            OnRawMouse(raw.data.mouse, m);
        return;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        if (!capture)
            return;
        const uint32_t dik = ((static_cast<uint32_t>(m->lParam) >> 16) & 0x7F) | ((m->lParam & (1 << 24)) ? 0x80u : 0u);
        if (IsW3Key(dik, screen))
            return;
        // Minecraft's key (its raw event already went to the ring). With a Minecraft screen open,
        // turn it into WM_CHAR first so typing reaches Minecraft as text.
        if (screen && (m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN) && U().TranslateMessage)
            U().TranslateMessage(m);
        Drop(m);
        return;
    }
    case WM_CHAR:
    case WM_SYSCHAR: {
        if (!capture)
            return;
        if (screen)
        {
            const wchar_t ch = static_cast<wchar_t>(m->wParam);
            if (ch >= 0xD800 && ch <= 0xDBFF)
            {
                g_highSurrogate = ch;
            }
            else if (ch >= 0xDC00 && ch <= 0xDFFF)
            {
                if (g_highSurrogate)
                    Queue(proto::kInText, 0, 0x10000 + ((g_highSurrogate - 0xD800) << 10) + (ch - 0xDC00));
                g_highSurrogate = 0;
            }
            else if (ch >= 0x20 && ch != 0x7F)
            {
                Queue(proto::kInText, 0, ch);
            }
            Drop(m);
        }
        return;
    }
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        if (capture)
            Drop(m);
        return;
    default:
        return;
    }
}

LRESULT CALLBACK GetMsgProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && wParam == PM_REMOVE && lParam)
        HandleMessage(reinterpret_cast<MSG*>(lParam));
    return U().CallNextHookEx ? U().CallNextHookEx(nullptr, code, wParam, lParam) : 0;
}

// ---- GetRawInputData import hook ------------------------------------------------------------
// W3 reads keyboard and mouse with GetRawInputData (witcher3.exe imports it from user32). The
// WH_GETMESSAGE hook alone missed everything in game (installed on the wrong thread during
// loading), so the raw data is taken where W3 itself reads it: every RAWINPUT W3 fetches passes
// through here, goes to Minecraft, and is blanked for W3 when Minecraft owns it.
using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
GetRawInputDataFn g_realGetRawInputData = nullptr;
std::atomic<DWORD> g_rawThread{0};
HRAWINPUT g_lastRawHandle = nullptr; // W3 may fetch one event twice: forward once, blank both
bool g_lastRawDropped = false;
bool g_lastRawRemapped = false;

// The interact key's event becomes W3's interaction key (same make/break; its scan code and VK).
void RemapToW3Interact(RAWINPUT* raw)
{
    if (raw->header.dwType != RIM_TYPEKEYBOARD)
        return;
    const uint32_t w3 = g_w3InteractDik.load(std::memory_order_relaxed);
    auto& kb = raw->data.keyboard;
    kb.MakeCode = static_cast<USHORT>(w3 & 0x7F);
    kb.Flags = static_cast<USHORT>((kb.Flags & RI_KEY_BREAK) | ((w3 & 0x80) ? RI_KEY_E0 : 0));
    UINT vk = 0;
    if (U().MapVirtualKeyW)
        vk = U().MapVirtualKeyW((w3 & 0x80) ? (0xE000u | (w3 & 0x7F)) : (w3 & 0x7F), 3 /* MAPVK_VSC_TO_VK_EX */);
    if (!vk && w3 == 0x12)
        vk = 'E';
    if (vk)
        kb.VKey = static_cast<USHORT>(vk);
}

void BlankRaw(RAWINPUT* raw)
{
    if (raw->header.dwType == RIM_TYPEMOUSE)
    {
        raw->data.mouse.lLastX = 0;
        raw->data.mouse.lLastY = 0;
        raw->data.mouse.usButtonFlags = 0;
        raw->data.mouse.usButtonData = 0;
    }
    else
    {
        raw->header.dwType = RIM_TYPEHID; // W3 ignores HID events
    }
}

void ProcessRaw(HRAWINPUT h, RAWINPUT* raw)
{
    if (h == g_lastRawHandle && h)
    {
        if (g_lastRawDropped)
            BlankRaw(raw);
        else if (g_lastRawRemapped)
            RemapToW3Interact(raw);
        return;
    }
    g_lastRawHandle = h;
    g_rawThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    const bool capture = CaptureNow();
    const bool screen = g_screenOpen.load(std::memory_order_relaxed);
    if (!capture && g_anyOwned)
    {
        std::fill(std::begin(g_ownedKey), std::end(g_ownedKey), false);
        std::fill(std::begin(g_ownedButton), std::end(g_ownedButton), false);
        g_anyOwned = false;
    }
    ResumeHeldKeys(capture, screen);
    t_dropRaw = false;
    t_remapRaw = false;
    if (raw->header.dwType == RIM_TYPEKEYBOARD)
        OnRawKeyboard(raw->data.keyboard, capture, screen, nullptr);
    else if (raw->header.dwType == RIM_TYPEMOUSE && capture)
        OnRawMouse(raw->data.mouse, nullptr);
    g_lastRawDropped = t_dropRaw;
    g_lastRawRemapped = !t_dropRaw && t_remapRaw;
    if (t_dropRaw)
        BlankRaw(raw);
    else if (t_remapRaw)
        RemapToW3Interact(raw);
}

UINT WINAPI Hook_GetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT headerSize)
{
    const UINT r = g_realGetRawInputData(h, cmd, data, size, headerSize);
    if (cmd == RID_INPUT && data && r != static_cast<UINT>(-1) && r >= sizeof(RAWINPUTHEADER))
    {
        __try
        {
            ProcessRaw(h, static_cast<RAWINPUT*>(data));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
    return r;
}

// Swaps witcher3.exe's IAT entry for user32!GetRawInputData. True once installed.
bool InstallRawHook()
{
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (!base)
        return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return false;
    for (auto* imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp)
    {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), "user32.dll") != 0)
            continue;
        auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots)
        {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
                continue;
            const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(byName->Name), "GetRawInputData") != 0)
                continue;
            DWORD old = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old))
                return false;
            g_realGetRawInputData = reinterpret_cast<GetRawInputDataFn>(slots->u1.Function);
            InterlockedExchangePointer(reinterpret_cast<PVOID*>(&slots->u1.Function), reinterpret_cast<PVOID>(&Hook_GetRawInputData));
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

// ---- Minecraft tick interpolation (SkyCraft Game.cpp, ported) -------------------------------
struct TickRec
{
    proto::McState s;
    int64_t at;
    int slots;
};

struct Interp
{
    std::deque<TickRec> history;
    int64_t lastFrameQpc = 0;
    int stampOutliers = 0;
    double renderDelayMs = 10.0;
    std::array<double, 40> tickDue{};
    size_t tickDueNext = 0;
    bool tickDueInit = false;
    float zoom = 0.0f;
    uint32_t zoomMode = 0;
    int64_t zoomQpc = 0;
    MotionStats motion;
};

std::mutex g_frameMutex; // guards g_interp, g_last
Interp g_interp;
Frame g_last;

int64_t QpcFreq()
{
    static const int64_t f = [] {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        return q.QuadPart;
    }();
    return f;
}

// Feet, eye height and walk bob at "now" on the QPC clock. Caller holds g_frameMutex.
void Interpolate(Interp& ip, const proto::McState& mc, double& fx, double& fy, double& fz, double& eyeH, float& bobPhase, float& bobAmount)
{
    fx = mc.x, fy = mc.y, fz = mc.z;
    eyeH = mc.eyeHeight > 0.0f ? mc.eyeHeight : 1.62;
    bobPhase = mc.bobPhase;
    bobAmount = mc.bobAmount;
    if (mc.tickQpc == 0 || mc.tickMs <= 0.0f)
        return;
    const double qpcPerMs = static_cast<double>(QpcFreq()) / 1000.0;
    const int64_t period = std::max<int64_t>(1, std::llround(static_cast<double>(mc.tickMs) * qpcPerMs));
    LARGE_INTEGER nowQ;
    QueryPerformanceCounter(&nowQ);
    const int64_t now = nowQ.QuadPart;
    if (ip.history.empty() || ip.history.back().s.tickQpc != mc.tickQpc)
    {
        if (!ip.history.empty() && mc.tickQpc < ip.history.back().s.tickQpc)
            ip.history.clear(); // Minecraft restarted
        TickRec tick{mc, mc.tickQpc, 1};
        if (!ip.history.empty())
        {
            auto& last = ip.history.back();
            const int64_t n = std::llround(static_cast<double>(mc.tickQpc - last.at) / static_cast<double>(period));
            const int64_t err = mc.tickQpc - (last.at + n * period);
            if (n == 0 && last.slots >= 2)
            {
                last.at -= period;
                last.slots -= 1;
                tick.at = last.at + period;
            }
            else if (n >= 1 && n <= 10 && std::llabs(err) < period * 3 / 10)
            {
                tick.at = last.at + n * period + err / 16;
                tick.slots = static_cast<int>(n);
                ip.stampOutliers = 0;
            }
            else if (n <= 10 && ++ip.stampOutliers < 3)
            {
                tick.slots = static_cast<int>(std::max<int64_t>(n, 1));
                tick.at = last.at + tick.slots * period;
            }
            else
            {
                ip.stampOutliers = 0;
            }
        }
        if (ip.lastFrameQpc != 0)
        {
            if (!ip.tickDueInit)
            {
                ip.tickDue.fill(ip.renderDelayMs - 1.0);
                ip.tickDueInit = true;
            }
            const double dueMs = static_cast<double>(ip.lastFrameQpc - tick.at) / qpcPerMs;
            if (dueMs < 30.0)
                ip.tickDue[ip.tickDueNext++ % ip.tickDue.size()] = dueMs;
        }
        ip.history.push_back(tick);
        if (ip.history.size() > 8)
            ip.history.pop_front();
        ++ip.motion.ticks;
    }
    const double frameMs = ip.lastFrameQpc != 0 ? static_cast<double>(now - ip.lastFrameQpc) / qpcPerMs : 0.0;
    ip.lastFrameQpc = now;
    if (ip.tickDueInit)
    {
        const double target = std::clamp(*std::max_element(ip.tickDue.begin(), ip.tickDue.end()) + 1.0, 4.0, 30.0);
        const double dt = std::min(frameMs, 100.0) / 1000.0;
        ip.renderDelayMs = target > ip.renderDelayMs ? std::min(target, ip.renderDelayMs + 20.0 * dt) : std::max(target, ip.renderDelayMs - 2.0 * dt);
    }
    const int64_t renderQpc = now - std::llround(ip.renderDelayMs * qpcPerMs);
    size_t i = 0;
    for (size_t k = ip.history.size(); k-- > 0;)
    {
        if (ip.history[k].at <= renderQpc)
        {
            i = k;
            break;
        }
    }
    const TickRec& tick = ip.history[i];
    const TickRec* next = i + 1 < ip.history.size() ? &ip.history[i + 1] : nullptr;
    const double ticks = static_cast<double>(renderQpc - tick.at) / static_cast<double>(period);
    const double t = std::clamp(ticks, 0.0, 1.0);
    const auto& s = tick.s;
    fx = s.prevX + (s.curX - s.prevX) * t;
    fy = s.prevY + (s.curY - s.prevY) * t;
    fz = s.prevZ + (s.curZ - s.prevZ) * t;
    eyeH = s.tickEyeO + (s.tickEye - s.tickEyeO) * t;
    bobPhase = -(s.walkDist + (s.walkDist - s.walkDistO) * static_cast<float>(t));
    bobAmount = s.bobO + (s.bob - s.bobO) * static_cast<float>(t);
    if (ticks > 1.0 && next)
    {
        const auto& n = next->s;
        const double gap = static_cast<double>(next->at - (tick.at + period));
        const double u = gap > 0.0 ? std::clamp(static_cast<double>(renderQpc - (tick.at + period)) / gap, 0.0, 1.0) : 1.0;
        fx = s.curX + (n.prevX - s.curX) * u;
        fy = s.curY + (n.prevY - s.curY) * u;
        fz = s.curZ + (n.prevZ - s.curZ) * u;
        eyeH = s.tickEye + (n.tickEyeO - s.tickEye) * u;
        const float endPhase = -(s.walkDist + (s.walkDist - s.walkDistO));
        bobPhase = endPhase + (-n.walkDist - endPhase) * static_cast<float>(u);
        bobAmount = s.bob + (n.bobO - s.bob) * static_cast<float>(u);
    }
    else if (ticks > 1.0)
    {
        ++ip.motion.lateFrames;
    }
    if (eyeH <= 0.0)
        eyeH = mc.eyeHeight > 0.0f ? mc.eyeHeight : 1.62;
    ++ip.motion.frames;
    ip.motion.renderDelayMs = ip.renderDelayMs;
}

float NormYaw(float yaw)
{
    float y = std::fmod(yaw, 360.0f);
    if (y < -180.0f)
        y += 360.0f;
    else if (y >= 180.0f)
        y -= 360.0f;
    return y;
}

// Script thread, once per game frame: Minecraft renders one frame per game frame (WcPace, see the
// protocol header) with this frame's look, instead of one per 16 ms host update.
// scriptFrame = false: the host thread keeps the panel pause fresh while The Witcher has paused the
// script (no pacing, no look: Minecraft then paces on SkyState, and SkyState carries the look).
void PublishPace(bool engaged, bool scriptFrame = true)
{
    uint8_t* base = host::MappingBase();
    if (!base)
        return;
    std::lock_guard lk(g_paceMutex);
    auto* p = reinterpret_cast<proto::WcPace*>(base + proto::kOffWcPace);
    std::atomic_ref<uint32_t> seq(p->seq);
    const uint32_t s = seq.load(std::memory_order_relaxed);
    seq.store(s | 1u, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    uint32_t flags = (g_mcPace.load() && scriptFrame) ? proto::kPaceActive : 0u;
    if (engaged && scriptFrame)
        flags |= proto::kPaceLook;
    if (g_panelActive.load())
        flags |= kPacePanel;
    p->flags = flags;
    p->gameFrame = camsync::GameFrames();
    p->tickMs = GetTickCount64();
    p->yaw = g_yaw.load();
    p->pitch = g_pitch.load();
    p->maxFps = static_cast<uint32_t>(g_mcMaxFps.load());
    seq.store((s | 1u) + 1u, std::memory_order_release);
}

// ---- host-thread state ---------------------------------------------------------------------
bool g_hostCapturing = false;
bool g_prevScreen = false;
bool g_prevDriven = false;
int g_cursorX = 0, g_cursorY = 0;
std::atomic<uint64_t> g_cursorPacked{0};
std::atomic<bool> g_cursorVisible{false};
uint32_t g_hookThread = 0;
bool g_prevMode = true;
} // namespace

bool McMode()
{
    return g_mcMode.load();
}

bool SensesHeld()
{
    const uint32_t f = g_wcFlags.load();
    const bool held = (f & proto::kWcValid) ? (f & proto::kWcSenses) != 0 : g_sensesHeld.load();
    return held && g_mcMode.load();
}

uint32_t WcFlags()
{
    return g_wcFlags.load();
}

uint32_t KeyDik(uint32_t which)
{
    const uint32_t keys = g_wcKeys.load();
    switch (which)
    {
    case proto::kWcKeyInteract:
        return g_interactDik.load() ? g_interactDik.load() : proto::kWcKeyNone;
    case proto::kWcKeyMode:
        return g_modeDik.load();
    case proto::kWcKeySenses:
        return (keys >> 16) & 0xFF ? (keys >> 16) & 0xFF : kDikV;
    case proto::kWcKeyRoach:
        return (keys >> 24) & 0xFF ? (keys >> 24) & 0xFF : kDikX;
    default:
        return 0;
    }
}

std::string KeyName(uint32_t dik)
{
    if (dik == proto::kWcKeyNone || dik == 0)
        return "none";
    if (dik == proto::kWcKeyMouse)
        return "a mouse button";
    wchar_t buf[64]{};
    const LONG lp = static_cast<LONG>(((dik & 0x7F) << 16) | ((dik & 0x80) ? (1u << 24) : 0u));
    const int n = U().GetKeyNameTextW ? U().GetKeyNameTextW(lp, buf, 64) : 0;
    if (n <= 0)
    {
        char hex[16];
        std::snprintf(hex, sizeof(hex), "key 0x%02X", dik);
        return hex;
    }
    std::string out;
    for (int i = 0; i < n; ++i)
        out += buf[i] < 0x80 ? static_cast<char>(buf[i]) : '?';
    return out;
}

void SetMcMode(bool on)
{
    if (g_mcMode.exchange(on) != on)
        WLOG("control: Minecraft mode %s", on ? "ON (Minecraft drives the player)" : "OFF (Witcher mode: Geralt drives, Minecraft mirrors)");
}

void ToggleMcMode()
{
    SetMcMode(!g_mcMode.load());
}

Frame OnFrame(uint32_t flags, float camHeadingDeg, float camPitchUpDeg)
{
    LoadIni();
    const uint64_t nowMs = GetTickCount64();
    const bool arrived = g_hostArrived.load();
    // Engage only once Minecraft's player has arrived where the host put it (else Geralt would be
    // moved to wherever Minecraft was left, e.g. by the last session); stay engaged through the
    // short holds of later teleports (rescues, W3-side jumps).
    // "Ready" (script ticks fresh, not loading) is only needed to engage: right after a W3 menu
    // closes the host may not have seen a fresh tick yet, and that mustn't flicker the body/camera.
    // M6a: Minecraft sometimes drops "in world" for a single host iteration (seen offline while it
    // types a chat command). That used to disengage for one frame: Geralt shown, his gravity back
    // on in mid-air, The Witcher's HUD flickering. Bridge drops shorter than 400 ms while engaged.
    static uint64_t s_mcLostMs = 0;
    static uint64_t s_mcBridged = 0;
    bool mcOk = g_hostMcOk.load();
    if (mcOk)
    {
        s_mcLostMs = 0;
    }
    else if (g_frameEngaged.load() && g_mcMode.load())
    {
        if (!s_mcLostMs)
        {
            s_mcLostMs = nowMs;
            if (++s_mcBridged <= 20)
                WLOG("control: Minecraft left its world for a moment; staying engaged for up to 400 ms (#%llu)",
                     static_cast<unsigned long long>(s_mcBridged));
        }
        mcOk = nowMs - s_mcLostMs < 400;
    }
    const bool base = g_mcMode.load() && mcOk && !(flags & kFrameSuspend);
    const bool engaged = base && (g_frameEngaged.load() || (arrived && g_hostReady.load()));
    const bool wasEngaged = g_frameEngaged.exchange(engaged);
    g_frameFlags.store(flags);
    const uint64_t prevFrameMs = g_lastFrameMs.exchange(nowMs);
    // Panels as Steve: safety nets in case the script never opens or never closes the panel state
    // (it normally does both: WCraft_Panel). A frame after a long gap = The Witcher's menu paused us.
    if (g_panelActive.load())
    {
        if (prevFrameMs && nowMs - prevFrameMs > 250)
            g_panelSawPause = true;
        if (!g_panelConfirmed.load() && nowMs - g_panelSinceMs.load() > 1500)
            EndPanel("the script didn't open it");
        else if (flags & kFrameNoCapture)
            g_panelNoMenuSinceMs = 0;
        else if (!g_panelNoMenuSinceMs.load())
            g_panelNoMenuSinceMs = nowMs;
        else if (g_panelConfirmed.load() && nowMs - g_panelNoMenuSinceMs.load() > 3000)
            EndPanel("no Witcher menu for 3 s");
    }
    if (engaged && !wasEngaged)
    {
        // Adopt the W3 camera's look so nothing jumps when Minecraft takes over.
        SetLook(180.0f - camHeadingDeg, std::clamp(-camPitchUpDeg, -90.0f, 90.0f));
        g_dx = 0;
        g_dy = 0;
        WLOG("control: engaged (Minecraft drives; look yaw %.1f pitch %.1f)", g_yaw.load(), g_pitch.load());
    }
    else if (!engaged && wasEngaged)
    {
        WLOG("control: disengaged (flags 0x%X, mode %d, mc %d, ready %d)", flags, g_mcMode.load() ? 1 : 0, g_hostMcOk.load() ? 1 : 0,
             g_hostReady.load() ? 1 : 0);
    }

    // Mouse look (Minecraft's formula, MouseHandler.turnPlayer), integrated here so the camera has
    // no added latency. A Minecraft screen takes the mouse for its cursor instead (host thread).
    if (engaged && !g_screenOpen.load() && !(flags & kFrameNoCapture))
    {
        const int dx = g_dx.exchange(0), dy = g_dy.exchange(0);
        if (dx || dy)
        {
            const float s = g_sensitivity.load() * 0.6f + 0.2f;
            const float factor = s * s * s * 8.0f * 0.15f;
            SetLook(g_yaw.load() + static_cast<float>(dx) * factor, g_pitch.load() + static_cast<float>(dy) * factor);
        }
    }

    Frame f;
    f.engaged = engaged;
    f.arrived = arrived;
    camsync::CamPose syncPose;
    proto::McState mc{};
    const bool haveMc = g_hostMcOk.load() && host::ReadMc(mc) && (mc.flags & proto::kMcInWorld);
    // Respawn guard: a dead Minecraft player, or one that jumped far by itself (a respawn at the
    // world spawn; not an ender pearl, not a teleport of ours), is no place for Geralt: until
    // Minecraft has put the new player back where the host says (SkyClient: a new player goes to
    // Geralt), its feet are not handed out (else Geralt follows it to the spawn and the host's
    // "W3 moved its player" rule then moves Steve there for good).
    {
        static double s_lastX = 0, s_lastY = 0, s_lastZ = 0, s_holdX = 0, s_holdY = 0, s_holdZ = 0;
        static bool s_haveLast = false;
        static uint64_t s_pearlMs = 0, s_notArrivedMs = 0, s_holdSince = 0;
        if (!arrived)
            s_notArrivedMs = nowMs;
        if (haveMc)
        {
            if (mc.pearlFlags & proto::kPearlInFlight)
                s_pearlMs = nowMs;
            const double jump = s_haveLast ? std::sqrt((mc.x - s_lastX) * (mc.x - s_lastX) + (mc.y - s_lastY) * (mc.y - s_lastY) +
                                                       (mc.z - s_lastZ) * (mc.z - s_lastZ))
                                           : 0.0;
            const bool dead = (mc.flags & proto::kMcDead) != 0;
            const bool selfJump = jump > 16.0 && nowMs - s_pearlMs > 1500 && nowMs - s_notArrivedMs > 1000;
            if (!s_holdSince && (dead || selfJump))
            {
                s_holdSince = nowMs;
                s_holdX = selfJump ? s_lastX : mc.x, s_holdY = selfJump ? s_lastY : mc.y, s_holdZ = selfJump ? s_lastZ : mc.z;
                WLOG("control: Minecraft's player %s; Geralt waits until it is back (%.1f %.1f %.1f)", dead ? "died" : "jumped far by itself",
                     s_holdX, s_holdY, s_holdZ);
            }
            if (s_holdSince)
            {
                const double back = std::sqrt((mc.x - s_holdX) * (mc.x - s_holdX) + (mc.y - s_holdY) * (mc.y - s_holdY) +
                                              (mc.z - s_holdZ) * (mc.z - s_holdZ));
                if ((!dead && back < 4.0) || nowMs - s_holdSince > 3000)
                {
                    WLOG("control: Minecraft's player is %s after %llu ms; Geralt follows it again", back < 4.0 ? "back" : "elsewhere",
                         static_cast<unsigned long long>(nowMs - s_holdSince));
                    s_holdSince = 0;
                }
            }
            s_lastX = mc.x, s_lastY = mc.y, s_lastZ = mc.z;
            s_haveLast = true;
        }
        else
        {
            s_haveLast = false;
        }
        if (s_holdSince)
            f.arrived = false;
    }
    std::lock_guard lk(g_frameMutex);
    if (haveMc)
    {
        double fx, fy, fz, eyeH;
        float bobPhase, bobAmount;
        Interpolate(g_interp, mc, fx, fy, fz, eyeH, bobPhase, bobAmount);
        const float yaw = g_yaw.load(), pitch = g_pitch.load();
        // Walk bob (GameRenderer.bobView) as a camera offset, like SkyCraft.
        const float phase = bobPhase * static_cast<float>(kPi);
        const float sideBlocks = -std::sin(phase) * bobAmount * 0.5f;
        const float liftBlocks = std::fabs(std::cos(phase) * bobAmount);
        const float bobPitchDeg = std::fabs(std::cos(phase - 0.2f) * bobAmount) * 5.0f;
        const float bobRollDeg = std::sin(phase) * bobAmount * 3.0f;

        // MC -> W3: (x, y, z) -> (x, -z, y). W3 heading h: forward (-sin h, cos h), right (cos h, sin h).
        const double heading = 180.0 - static_cast<double>(yaw);
        const double hr = heading * kDeg;
        double ex = fx + std::cos(hr) * sideBlocks;
        double ey = -fz + std::sin(hr) * sideBlocks;
        double ez = fy + eyeH + liftBlocks;

        // Minecraft's F5 camera: behind the player, or in front looking back (mode 2).
        const bool detached = mc.cameraMode != 0 && mc.cameraDistance > 0.0f;
        // Pull in at once (collision), ease back out over ~0.2 s whatever the frame rate (SkyCraft).
        LARGE_INTEGER zq;
        QueryPerformanceCounter(&zq);
        const double zoomDt =
            g_interp.zoomQpc ? std::min(static_cast<double>(zq.QuadPart - g_interp.zoomQpc) / static_cast<double>(QpcFreq()), 0.1) : 0.0;
        g_interp.zoomQpc = zq.QuadPart;
        if (!detached || g_interp.zoomMode != mc.cameraMode || mc.cameraDistance < g_interp.zoom)
            g_interp.zoom = detached ? mc.cameraDistance : 0.0f;
        else
            g_interp.zoom += (mc.cameraDistance - g_interp.zoom) * static_cast<float>(1.0 - std::exp(-zoomDt / 0.2));
        g_interp.zoomMode = mc.cameraMode;
        const bool mirrored = mc.cameraMode == 2;
        const double camHeading = mirrored ? heading + 180.0 : heading;
        const double lookPitchUp = mirrored ? static_cast<double>(pitch) : -static_cast<double>(pitch);
        if (detached)
        {
            const double ch = camHeading * kDeg, cp = lookPitchUp * kDeg;
            const double fwdX = -std::sin(ch) * std::cos(cp), fwdY = std::cos(ch) * std::cos(cp), fwdZ = std::sin(cp);
            ex -= fwdX * g_interp.zoom;
            ey -= fwdY * g_interp.zoom;
            ez -= fwdZ * g_interp.zoom;
        }
        f.eyeValid = true;
        f.eye[0] = ex, f.eye[1] = ey, f.eye[2] = ez;
        f.feet[0] = fx, f.feet[1] = -fz, f.feet[2] = fy;
        double hdg = std::fmod(camHeading, 360.0);
        if (hdg > 180.0)
            hdg -= 360.0;
        else if (hdg <= -180.0)
            hdg += 360.0;
        f.headingDeg = static_cast<float>(hdg);
        f.pitchUpDeg = static_cast<float>(lookPitchUp) - bobPitchDeg;
        f.rollDeg = bobRollDeg;
        f.mcFovDeg = mc.fovDeg;
        f.cameraMode = mc.cameraMode;
        double bh = std::fmod(heading, 360.0);
        if (bh > 180.0)
            bh -= 360.0;
        else if (bh <= -180.0)
            bh += 360.0;
        f.bodyHeadingDeg = static_cast<float>(bh);
        f.lookPitchUpDeg = -pitch;
        f.eyeHeight = static_cast<float>(eyeH);

        // World layer lock (worldlayer.md): the camera of the overlay frame being drawn, which already
        // has the bob and F5 in it. Only once Minecraft's player has arrived, the frame is fresh and
        // its FOV sane; otherwise SkyCraft's interpolation above.
        OverlayPose pose;
        const PoseProvider provider = g_poseProvider.load();
        if (g_usePose.load() == 1 && provider && arrived && provider(pose) && pose.fovDeg >= 10.0f && nowMs - pose.takenMs < 150)
        {
            f.eye[0] = pose.eye[0];
            f.eye[1] = -pose.eye[2];
            f.eye[2] = pose.eye[1];
            double ph = 180.0 - static_cast<double>(pose.yaw);
            ph = std::fmod(ph, 360.0);
            if (ph > 180.0)
                ph -= 360.0;
            else if (ph <= -180.0)
                ph += 360.0;
            f.headingDeg = static_cast<float>(ph);
            f.pitchUpDeg = -pose.pitch;
            f.rollDeg = pose.rollDeg;
            f.mcFovDeg = pose.fovDeg;
            f.fromPose = true;
            ++g_poseFrames;
            syncPose.frameId = pose.frameId;
            syncPose.eye[0] = pose.eye[0], syncPose.eye[1] = pose.eye[1], syncPose.eye[2] = pose.eye[2];
            syncPose.yaw = pose.yaw, syncPose.pitch = pose.pitch, syncPose.roll = pose.rollDeg, syncPose.fov = pose.fovDeg;
        }
        else
        {
            // The interpolated camera, back in Minecraft's convention (W3 (x, y, z) = MC (x, -z, y)).
            syncPose.eye[0] = f.eye[0], syncPose.eye[1] = f.eye[2], syncPose.eye[2] = -f.eye[1];
            syncPose.yaw = NormYaw(180.0f - f.headingDeg);
            syncPose.pitch = -f.pitchUpDeg;
            syncPose.roll = f.rollDeg;
            syncPose.fov = f.mcFovDeg;
        }
        f.fovDeg = ConvertFov(f.mcFovDeg);
    }
    else
    {
        g_interp.history.clear();
    }
    g_last = f;
    // Visual sync: remember which camera this game frame got, so the compositor can
    // draw the Minecraft frame W3 renders this frame with when it is presented.
    camsync::OnGameFrame(engaged && f.eyeValid, syncPose);
    PublishPace(engaged);
    return f;
}

Frame LastFrame()
{
    std::lock_guard lk(g_frameMutex);
    return g_last;
}

void HostUpdate(const proto::McState* mc, bool mcAlive, bool inGameReady, uint32_t viewportW, uint32_t viewportH)
{
    const uint64_t now = GetTickCount64();
    const bool inWorld = mcAlive && mc && (mc->flags & proto::kMcInWorld);
    g_hostMcOk.store(inWorld);
    g_vpW.store(viewportW);
    g_vpH.store(viewportH);
    g_hostReady.store(inGameReady);
    if (inWorld && mc->sensitivity > 0.0f)
        g_sensitivity.store(mc->sensitivity);
    // Minecraft's WitcherCraft options (its Esc menu): HUD pieces etc. for the script, the interact key.
    if (inWorld && (mc->wcFlags & proto::kWcValid))
    {
        g_wcFlags.store(mc->wcFlags);
        const uint32_t keys = mc->wcKeys;
        if (g_wcKeys.exchange(keys) != keys)
        {
            // Interact: 0 = keep (the ini's), none / a mouse button = off (the host reads keys only).
            uint32_t key = keys & 0xFF;
            if (key == proto::kWcKeyNone || key == proto::kWcKeyMouse)
                key = 0;
            else if (key == 0)
                key = g_interactDik.load();
            if (key != g_interactDik.load())
            {
                g_interactDik.store(key);
                WLOG("control: interact key -> %s (Minecraft's WitcherCraft controls)", key ? KeyName(key).c_str() : "none");
            }
            // The Steve / Geralt switch can't be unbound (nothing else gets you back to Minecraft).
            uint32_t mode = (keys >> 8) & 0xFF;
            if (mode == 0 || mode == proto::kWcKeyNone || mode == proto::kWcKeyMouse || mode == kDikEsc)
                mode = kDikF8;
            if (mode != g_modeDik.load())
            {
                g_modeDik.store(mode);
                WLOG("control: Steve / Geralt switch key -> %s (Minecraft's WitcherCraft controls)", KeyName(mode).c_str());
            }
        }
    }

    // Panels as Steve: The Witcher's open menu has paused the script. Keep its ticks alive with
    // Geralt's position as it opened the panel (he can't move while paused) and "no follow", so the
    // host doesn't treat the pause as a load (its teleport on resume put Steve onto grounded Geralt),
    // and keep Minecraft's pause request fresh. The look ticked is Steve's own (it is what the host
    // publishes while it doesn't drive), so nothing turns.
    if (g_panelActive.load() && now - g_lastFrameMs.load() > kPanelReTickMs)
    {
        if (now - g_lastFrameMs.load() > 250)
            g_panelSawPause = true;
        if (g_panelConfirmed.load())
        {
            float pos[3];
            {
                std::lock_guard lk(g_panelMutex);
                std::memcpy(pos, g_panelPos, sizeof(pos));
            }
            host::Tick(pos[0], pos[1], pos[2], 180.0f - g_yaw.load(), -g_pitch.load(), host::kTickInGame | host::kTickNoFollow);
            if (++g_panelReTicks == 1 || g_panelReTicks % 3000 == 0)
                WLOG("control: The Witcher's panel paused its scripts; ticking for them (Geralt at %.1f %.1f %.1f, #%llu)", pos[0], pos[1], pos[2],
                     static_cast<unsigned long long>(g_panelReTicks.load()));
        }
        PublishPace(false, false);
    }

    // Raw input is taken where W3 reads it (GetRawInputData import hook).
    if (!g_rawHookTried)
    {
        g_rawHookTried = true;
        const bool ok = InstallRawHook();
        g_rawHooked.store(ok);
        WLOG("control: GetRawInputData import hook %s", ok ? "installed" : "FAILED (falling back to the message hook)");
    }
    // The message hook (typing as WM_CHAR, swallowing legacy key/mouse messages) must sit on the
    // thread that really pumps W3's input: the one calling GetRawInputData. Move it there if the
    // first guess (foreground window during loading) was another thread.
    if (g_hook && U().ok)
    {
        const DWORD rawTid = g_rawThread.load(std::memory_order_relaxed);
        if (rawTid && rawTid != g_hookThread)
        {
            if (U().UnhookWindowsHookEx)
                U().UnhookWindowsHookEx(g_hook);
            g_hook = U().SetWindowsHookExW(WH_GETMESSAGE, GetMsgProc, nullptr, rawTid);
            WLOG("control: input hook moved from thread %lu to W3's input thread %lu %s", g_hookThread, rawTid, g_hook ? "ok" : "FAILED");
            g_hookThread = rawTid;
        }
    }
    // Install the input hook on the thread that owns the game window (first time it has focus).
    if (!g_hook && U().ok)
    {
        HWND fg = U().GetForegroundWindow();
        DWORD pid = 0;
        const DWORD tid = fg ? U().GetWindowThreadProcessId(fg, &pid) : 0;
        if (tid && pid == GetCurrentProcessId())
        {
            g_hook = U().SetWindowsHookExW(WH_GETMESSAGE, GetMsgProc, nullptr, tid);
            g_hookThread = tid;
            WLOG("control: input hook on game window thread %lu %s", tid, g_hook ? "installed" : "FAILED");
        }
    }

    const bool frameFresh = now - g_lastFrameMs.load() < kDrivenFreshMs;
    const bool driven = g_mcMode.load() && inWorld && inGameReady && frameFresh && g_frameEngaged.load();
    g_driven.store(driven);
    if (driven != g_prevDriven)
    {
        WLOG("control: host %s", driven ? "MC-driven (no mirror teleports; look from the mouse)" : "mirror mode (Minecraft follows Geralt)");
        g_prevDriven = driven;
    }
    const bool mode = g_mcMode.load();
    if (mode != g_prevMode)
    {
        g_prevMode = mode;
        if (!mode)
            buildmode::QueueEvent({proto::kInReleaseAll, 0, 0, 0, 0});
    }

    // Minecraft screens: the mouse drives Minecraft's cursor (overlay pixels).
    const bool screen = inWorld && (mc->flags & proto::kMcScreenOpen);
    g_screenOpen.store(screen);
    const int vw = viewportW ? static_cast<int>(viewportW) : 854, vh = viewportH ? static_cast<int>(viewportH) : 480;
    const bool capture = Capturing();
    if (screen && !g_prevScreen)
    {
        g_cursorX = vw / 2;
        g_cursorY = vh / 2;
        g_dx = 0;
        g_dy = 0;
        if (capture)
            Queue(proto::kInCursor, 0, g_cursorX, g_cursorY);
        WLOG("control: Minecraft screen open: mouse -> Minecraft cursor (%dx%d)", vw, vh);
    }
    else if (!screen && g_prevScreen)
    {
        g_dx = 0;
        g_dy = 0;
        WLOG("control: Minecraft screen closed: mouse -> look");
    }
    g_prevScreen = screen;
    if (screen && capture)
    {
        const int dx = g_dx.exchange(0), dy = g_dy.exchange(0);
        if (dx || dy)
        {
            g_cursorX = std::clamp(g_cursorX + dx, 0, vw - 1);
            g_cursorY = std::clamp(g_cursorY + dy, 0, vh - 1);
            Queue(proto::kInCursor, 0, g_cursorX, g_cursorY);
        }
    }
    g_cursorPacked.store((static_cast<uint64_t>(static_cast<uint32_t>(g_cursorX)) << 32) | static_cast<uint32_t>(g_cursorY));
    g_cursorVisible.store(screen && capture);

    if (g_hostCapturing && !capture)
    {
        Queue(proto::kInReleaseAll, 0, 0);
        g_dx = 0;
        g_dy = 0;
    }
    if (capture != g_hostCapturing)
        WLOG("control: input capture %s", capture ? "ON (keys/mouse -> Minecraft)" : "OFF (input -> The Witcher)");
    g_hostCapturing = capture;
}

void SetArrived(bool arrived)
{
    g_hostArrived.store(arrived);
}

bool Driven()
{
    return g_driven.load();
}

void GetLook(float& yaw, float& pitch)
{
    yaw = g_yaw.load();
    pitch = g_pitch.load();
}

void SetLook(float yaw, float pitch)
{
    g_yaw.store(NormYaw(yaw));
    g_pitch.store(std::clamp(pitch, -90.0f, 90.0f));
}

bool Capturing()
{
    return CaptureNow();
}

void SetFovMode(int mode)
{
    LoadIni();
    g_fovMode.store(std::clamp(mode, 0, 2));
    WLOG("control: FOV mode %d (0 vertical, 1 horizontal at the real aspect, 2 horizontal at 4:3)", g_fovMode.load());
}

int FovMode()
{
    LoadIni();
    return g_fovMode.load();
}

void SetUsePose(bool on)
{
    LoadIni();
    g_usePose.store(on ? 1 : 0);
    WLOG("control: camera source %s", on ? "overlay frame pose (world layer lock)" : "tick interpolation (SkyCraft)");
}

bool UsePose()
{
    LoadIni();
    return g_usePose.load() == 1;
}

void SetMcPace(int pace, int maxFps)
{
    LoadIni();
    g_mcPace = pace ? 1 : 0;
    g_mcMaxFps = std::clamp(maxFps, 0, 1000);
    WLOG("control: McPace=%d McMaxFps=%d", g_mcPace.load(), g_mcMaxFps.load());
}

void SetPoseProvider(PoseProvider fn)
{
    g_poseProvider.store(fn);
}

bool Cursor(int& x, int& y)
{
    const uint64_t p = g_cursorPacked.load();
    x = static_cast<int32_t>(static_cast<uint32_t>(p >> 32));
    y = static_cast<int32_t>(static_cast<uint32_t>(p));
    return g_cursorVisible.load();
}

bool IdentityKeys()
{
    return IdentityNow();
}

uint32_t ConsumeW3MenuRequest()
{
    return g_w3MenuReq.exchange(0);
}

bool GeraltGrounded()
{
    return g_frameEngaged.load(std::memory_order_relaxed) && (g_frameFlags.load(std::memory_order_relaxed) & kFrameGrounded) &&
           g_mcMode.load(std::memory_order_relaxed);
}

void PanelBegin(float x, float y, float z)
{
    {
        std::lock_guard lk(g_panelMutex);
        g_panelPos[0] = x, g_panelPos[1] = y, g_panelPos[2] = z;
    }
    if (!g_panelActive.load())
    {
        WLOG("control: the script opens a Witcher panel, but the panel state already ended");
        return;
    }
    g_panelConfirmed = true;
    WLOG("control: the script opened the Witcher panel (DIK 0x%02X); Geralt stays hidden at %.1f %.1f %.1f", g_panelKey.load(), x, y, z);
}

void PanelEnd(const char* why)
{
    EndPanel(why);
}

uint32_t PanelState()
{
    return (g_panelActive.load() ? 1u : 0u) | (g_panelSawPause.load() ? 2u : 0u) | (g_panelConfirmed.load() ? 4u : 0u);
}

bool OverlayWanted()
{
    if (g_panelActive.load(std::memory_order_relaxed))
        return false; // The Witcher's panel is on screen (Minecraft is paused under it)
    if (g_mcMode.load(std::memory_order_relaxed))
        return !(g_frameFlags.load(std::memory_order_relaxed) & kFrameSuspend);
    return buildmode::Enabled();
}

InteractPoll ConsumeInteract()
{
    InteractPoll p;
    p.presses = g_interactPresses.exchange(0);
    p.givenToW3 = g_interactGiven.exchange(0);
    p.held = g_interactHeld.load();
    p.dik = g_interactDik.load();
    return p;
}

void TestRawInput(void* rawInput, void* handle)
{
    if (rawInput)
        ProcessRaw(static_cast<HRAWINPUT>(handle), static_cast<RAWINPUT*>(rawInput));
}

InputStats Inputs()
{
    InputStats s;
    s.keysSent = g_keysSent.load();
    s.keysPassed = g_keysPassed.load();
    s.interactPresses = g_interactTotal.load();
    s.interactRemapped = g_interactRemapTotal.load();
    s.mcMenuOpens = g_mcMenuOpens.load();
    s.w3MenuSwitch = g_w3MenuSwitch.load();
    s.w3Panels = g_panelOpens.load();
    s.panelReTicks = g_panelReTicks.load();
    return s;
}

void InjectMouse(int dx, int dy)
{
    g_dx += dx;
    g_dy += dy;
}

MotionStats Motion()
{
    std::lock_guard lk(g_frameMutex);
    return g_interp.motion;
}

std::string StatusSuffix()
{
    char buf[256];
    std::snprintf(buf, sizeof(buf), " | mode=%s%s%s look=(%.0f,%.0f) keys=%llu fovmode=%d pose=%d/%llu", g_mcMode.load() ? "MC" : "witcher",
                  g_driven.load() ? " driven" : "", Capturing() ? " capture" : "", g_yaw.load(), g_pitch.load(),
                  static_cast<unsigned long long>(g_keysSent.load()), g_fovMode.load(), g_usePose.load(),
                  static_cast<unsigned long long>(g_poseFrames.load()));
    return std::string(buf) + " pace=" + std::to_string(g_mcPace.load()) + " lat=" + std::to_string(camsync::LatencyInUse()) +
           " interact=" + std::to_string(g_interactTotal.load()) + "/" + std::to_string(g_interactRemapTotal.load()) +
           " identity=" + std::to_string(IdentityNow() ? 1 : 0) + " mcmenu=" + std::to_string(g_mcMenuOpens.load()) +
           " w3panel=" + std::to_string(g_w3MenuSwitch.load()) + " panels=" + std::to_string(g_panelOpens.load()) +
           (g_panelActive.load() ? "(open)" : "") + " grounded=" + std::to_string(GeraltGrounded() ? 1 : 0);
}
} // namespace wc::control
