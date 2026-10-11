// WitcherCraft M2b build mode. See buildmode.hpp.
#include "buildmode.hpp"

#include "blocks.hpp"
#include "control.hpp"
#include "log.hpp"
#include "skycraft_protocol.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <type_traits>
#include <vector>

namespace wc::buildmode
{
namespace proto = skycraft::proto;

namespace
{
template <class T>
std::atomic_ref<T> A(T& v)
{
    return std::atomic_ref<T>(v);
}

constexpr uint64_t kAimFreshMs = 300;   // aim older than this is ignored (script stopped aiming)
constexpr double kAimRange = 30.0;      // metres along the camera ray
constexpr uint16_t kSdlSlash = 56;      // SDL_SCANCODE_SLASH: opens MC's command screen with "/"
constexpr uint16_t kSdlReturn = 40;     // SDL_SCANCODE_RETURN
constexpr uint64_t kScreenTimeoutMs = 2000;

// What we forward while build mode is on: Windows virtual key -> MC input.
struct Binding
{
    int vk;
    uint16_t type; // proto::kInKey (SDL scancode) or proto::kInMouseButton (SDL button)
    uint16_t code;
};
constexpr Binding kBindings[] = {
    {VK_LBUTTON, proto::kInMouseButton, 1}, // attack / break
    {VK_RBUTTON, proto::kInMouseButton, 3}, // use / place
    {VK_MBUTTON, proto::kInMouseButton, 2}, // pick block (creative)
    {'1', proto::kInKey, 30}, {'2', proto::kInKey, 31}, {'3', proto::kInKey, 32}, {'4', proto::kInKey, 33}, {'5', proto::kInKey, 34},
    {'6', proto::kInKey, 35}, {'7', proto::kInKey, 36}, {'8', proto::kInKey, 37}, {'9', proto::kInKey, 38},
    {'E', proto::kInKey, 8},                // inventory (shows once the HUD overlay lands)
};
constexpr size_t kBindingCount = sizeof(kBindings) / sizeof(kBindings[0]);

// The kit: creative mode plus a hotbar that covers every colour class (idempotent commands).
const char* const kKit[] = {
    "gamemode creative",
    "item replace entity @s hotbar.0 with minecraft:stone 64",
    "item replace entity @s hotbar.1 with minecraft:oak_planks 64",
    "item replace entity @s hotbar.2 with minecraft:sand 64",
    "item replace entity @s hotbar.3 with minecraft:grass_block 64",
    "item replace entity @s hotbar.4 with minecraft:obsidian 64",
    "item replace entity @s hotbar.5 with minecraft:bricks 64",
    "item replace entity @s hotbar.6 with minecraft:cobblestone 64",
    "item replace entity @s hotbar.7 with minecraft:oak_log 64",
    "item replace entity @s hotbar.8 with minecraft:glass 64",
};

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_kitRequested{false};

std::mutex g_mutex; // guards g_events, g_commands, g_aim
std::vector<proto::InputEvent> g_events;
std::deque<std::string> g_commands;

struct Aim
{
    bool active = false;
    double x = 0, y = 0, z = 0; // MC coords
    uint64_t ms = 0;
};
Aim g_aim;

// user32 input functions, resolved at run time like the overlay's d3d12/dxgi (no new static
// imports in dinput8.dll / fake_host.exe; Smart App Control on the dev machine blocked the
// fake_host build that imported them statically). user32 is always loaded in the game.
struct User32
{
    SHORT(WINAPI* GetAsyncKeyState)(int) = nullptr;
    HHOOK(WINAPI* SetWindowsHookExW)(int, HOOKPROC, HINSTANCE, DWORD) = nullptr;
    LRESULT(WINAPI* CallNextHookEx)(HHOOK, int, WPARAM, LPARAM) = nullptr;
    UINT(WINAPI* GetRawInputData)(HRAWINPUT, UINT, LPVOID, PUINT, UINT) = nullptr;
    HWND(WINAPI* GetForegroundWindow)() = nullptr;
    DWORD(WINAPI* GetWindowThreadProcessId)(HWND, LPDWORD) = nullptr;
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
        get(r.GetAsyncKeyState, "GetAsyncKeyState");
        get(r.SetWindowsHookExW, "SetWindowsHookExW");
        get(r.CallNextHookEx, "CallNextHookEx");
        get(r.GetRawInputData, "GetRawInputData");
        get(r.GetForegroundWindow, "GetForegroundWindow");
        get(r.GetWindowThreadProcessId, "GetWindowThreadProcessId");
        r.ok = r.GetAsyncKeyState && r.GetForegroundWindow && r.GetWindowThreadProcessId;
        return r;
    }();
    return u;
}

// Wheel from the game thread's message queue (WH_GETMESSAGE hook on that one thread).
std::atomic<int> g_wheelLegacy{0}, g_wheelRaw{0};
std::atomic<uint64_t> g_lastRawWheelMs{0};
HHOOK g_hook = nullptr;

LRESULT CALLBACK GetMsgProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && wParam == PM_REMOVE && g_enabled.load(std::memory_order_relaxed))
    {
        const MSG* m = reinterpret_cast<const MSG*>(lParam);
        if (m->message == WM_MOUSEWHEEL)
        {
            g_wheelLegacy += GET_WHEEL_DELTA_WPARAM(m->wParam);
        }
        else if (m->message == WM_INPUT)
        {
            RAWINPUT raw;
            UINT size = sizeof(raw);
            if (U().GetRawInputData && U().GetRawInputData(reinterpret_cast<HRAWINPUT>(m->lParam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1) &&
                raw.header.dwType == RIM_TYPEMOUSE && (raw.data.mouse.usButtonFlags & RI_MOUSE_WHEEL))
            {
                g_wheelRaw += static_cast<SHORT>(raw.data.mouse.usButtonData);
                g_lastRawWheelMs = GetTickCount64();
            }
        }
    }
    return U().CallNextHookEx ? U().CallNextHookEx(nullptr, code, wParam, lParam) : 0;
}

bool OwnsForeground(DWORD* threadOut)
{
    if (!U().ok)
        return false;
    HWND fg = U().GetForegroundWindow();
    if (!fg)
        return false;
    DWORD pid = 0;
    const DWORD tid = U().GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId())
        return false;
    if (threadOut)
        *threadOut = tid;
    return true;
}

void WriteRing(uint8_t* base, const proto::InputEvent& ev, uint64_t& dropped)
{
    auto* ring = base + proto::kOffInputRing;
    auto& headRef = *reinterpret_cast<uint64_t*>(ring + proto::kInputRingHeadOff);
    auto& tailRef = *reinterpret_cast<uint64_t*>(ring + proto::kInputRingTailOff);
    const uint64_t head = A(headRef).load(std::memory_order_relaxed);
    const uint64_t tail = A(tailRef).load(std::memory_order_acquire);
    if (head - tail >= proto::kInputRingEntries)
    {
        ++dropped;
        return;
    }
    reinterpret_cast<proto::InputEvent*>(ring + proto::kInputRingDataOff)[head & (proto::kInputRingEntries - 1)] = ev;
    A(headRef).store(head + 1, std::memory_order_release);
}

// ---- host-thread state ---------------------------------------------------------------------
bool g_prevDown[kBindingCount] = {};
bool g_prevToggle = false;
bool g_capturing = false;
uint32_t g_kitPid = 0;
uint64_t g_dropped = 0, g_eventsSent = 0, g_commandsDone = 0;
int g_clickLogs = 0;

enum class Typer
{
    Idle,
    WaitScreen,
    Typed,
    WaitClose,
};
std::atomic<Typer> g_typer{Typer::Idle};
uint64_t g_typerMs = 0;
int g_typerTries = 0;
std::string g_typing;
} // namespace

bool Enabled()
{
    return g_enabled.load();
}

void SetEnabled(bool on)
{
    if (g_enabled.exchange(on) != on)
        WLOG("build mode %s", on ? "ON" : "OFF");
}

void Toggle()
{
    SetEnabled(!g_enabled.load());
}

void QueueInput(uint16_t type, uint16_t code, int32_t a)
{
    std::lock_guard lk(g_mutex);
    g_events.push_back({type, code, a, 0, 0});
}

void QueueEvent(const proto::InputEvent& ev)
{
    std::lock_guard lk(g_mutex);
    if (g_events.size() < proto::kInputRingEntries)
        g_events.push_back(ev);
}

void QueueCommand(const std::string& command)
{
    std::string c = command;
    while (!c.empty() && (c.front() == '/' || c.front() == ' '))
        c.erase(c.begin());
    if (c.empty())
        return;
    std::lock_guard lk(g_mutex);
    g_commands.push_back(c);
}

size_t CommandsPending()
{
    std::lock_guard lk(g_mutex);
    return g_commands.size() + (g_typer != Typer::Idle ? 1 : 0);
}

void ResendKit()
{
    g_kitRequested = true;
}

Vec4 SetAim(bool active, const float camPos[3], const float camDir[3], const float hit[3], bool hitValid)
{
    if (!active)
    {
        std::lock_guard lk(g_mutex);
        g_aim.active = false;
        return {0, 0, 0, 0};
    }
    // W3 -> MC: (x, y, z) -> (x, z, -y)
    const double o[3] = {camPos[0], camPos[2], -static_cast<double>(camPos[1])};
    double d[3] = {camDir[0], camDir[2], -static_cast<double>(camDir[1])};
    const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1e-6)
        return {0, 0, 0, 0};
    for (double& c : d)
        c /= len;
    double t = kAimRange;
    bool any = false;
    if (hitValid)
    {
        const double h[3] = {hit[0], hit[2], -static_cast<double>(hit[1])};
        const double th = (h[0] - o[0]) * d[0] + (h[1] - o[1]) * d[1] + (h[2] - o[2]) * d[2];
        if (th > 0.0 && th < t)
        {
            t = th;
            any = true;
        }
    }
    double tb;
    if (blocks::Raycast(o, d, t, tb))
    {
        t = tb;
        any = true;
    }
    const double tx = o[0] + d[0] * t, ty = o[1] + d[1] * t, tz = o[2] + d[2] * t;
    {
        std::lock_guard lk(g_mutex);
        g_aim = {true, tx, ty, tz, GetTickCount64()};
    }
    return {static_cast<float>(tx), static_cast<float>(-tz), static_cast<float>(ty), any ? 1.0f : 0.0f};
}

bool AimLook(const proto::McState* mc, double feetX, double feetY, double feetZ, float& yaw, float& pitch)
{
    Aim aim;
    {
        std::lock_guard lk(g_mutex);
        aim = g_aim;
    }
    if (!g_enabled.load() || !aim.active || GetTickCount64() - aim.ms > kAimFreshMs)
        return false;
    double ex = feetX, ey = feetY + 1.62, ez = feetZ;
    if (mc && (mc->flags & proto::kMcInWorld) && (mc->eyeX != 0 || mc->eyeY != 0 || mc->eyeZ != 0))
    {
        // MC raycasts from its own eye; it follows the host within ~0.2 m.
        ex = mc->eyeX, ey = mc->eyeY, ez = mc->eyeZ;
    }
    const double dx = aim.x - ex, dy = aim.y - ey, dz = aim.z - ez;
    const double horiz = std::sqrt(dx * dx + dz * dz);
    if (horiz * horiz + dy * dy < 0.09)
        return false;
    // MC: forward = (-sin yaw, cos yaw) in x/z; pitch positive looking down.
    yaw = static_cast<float>(std::atan2(-dx, dz) * 57.29577951308232);
    pitch = static_cast<float>(std::clamp(-std::atan2(dy, horiz) * 57.29577951308232, -90.0, 90.0));
    return true;
}

void HostPump(uint8_t* base, bool mcAlive, const proto::McState* mc, uint32_t mcPid, bool scriptTicking)
{
    if (!base)
        return;
    const uint64_t now = GetTickCount64();
    std::vector<proto::InputEvent> out;

    // ---- keyboard / mouse (only while W3 has focus and is running) ----
    DWORD gameThread = 0;
    const bool foreground = OwnsForeground(&gameThread);
    const bool toggleDown = foreground && (U().GetAsyncKeyState(kToggleVk) & 0x8000) != 0;
    // M3a: in Minecraft mode every click already goes to Minecraft (control.cpp), so F7 build
    // mode only exists in Witcher mode (F8).
    const bool mcMode = control::McMode();
    if (mcMode && g_enabled.load())
    {
        SetEnabled(false);
        std::lock_guard lk(g_mutex);
        g_aim.active = false;
    }
    if (toggleDown && !g_prevToggle && scriptTicking && !mcMode)
        Toggle();
    g_prevToggle = toggleDown;

    const bool capture = g_enabled.load() && foreground && scriptTicking && mcAlive;
    if (capture)
    {
        if (!g_hook && gameThread && U().SetWindowsHookExW)
        {
            g_hook = U().SetWindowsHookExW(WH_GETMESSAGE, GetMsgProc, nullptr, gameThread);
            WLOG("build mode: wheel hook on game thread %lu %s", gameThread, g_hook ? "installed" : "FAILED");
        }
        for (size_t i = 0; i < kBindingCount; ++i)
        {
            const bool down = (U().GetAsyncKeyState(kBindings[i].vk) & 0x8000) != 0;
            if (down != g_prevDown[i])
            {
                out.push_back({kBindings[i].type, kBindings[i].code, down ? 1 : 0, 0, 0});
                if (kBindings[i].type == proto::kInMouseButton && down && g_clickLogs++ < 30)
                    WLOG("build mode: mouse %u down -> Minecraft", kBindings[i].code);
            }
            g_prevDown[i] = down;
        }
        // Wheel: raw input if the game uses it (it also gets legacy messages otherwise).
        const int raw = g_wheelRaw.exchange(0), legacy = g_wheelLegacy.exchange(0);
        const int wheel = (now - g_lastRawWheelMs.load() < 1000) ? raw : legacy;
        if (wheel != 0)
            out.push_back({proto::kInScroll, 0, wheel, 0, 0});
    }
    else
    {
        g_wheelRaw = 0;
        g_wheelLegacy = 0;
        if (g_capturing)
            out.push_back({proto::kInReleaseAll, 0, 0, 0, 0});
        std::fill(std::begin(g_prevDown), std::end(g_prevDown), false);
    }
    g_capturing = capture;

    // ---- starter kit, once per Minecraft process (or on request) ----
    const bool inWorld = mcAlive && mc && (mc->flags & proto::kMcInWorld);
    if (g_enabled.load() && inWorld && (g_kitPid != mcPid || g_kitRequested.exchange(false)))
    {
        g_kitPid = mcPid;
        for (const char* c : kKit)
            QueueCommand(c);
        WLOG("build mode: giving the Minecraft player its kit (creative + hotbar), %zu commands", std::size(kKit));
    }
    // The flight kit (elytra + rockets) and the hotbar kit are given by the Minecraft mod itself on
    // join (SkyCraft.giveFlightKit / giveWitcherHotbar): typed commands flashed the chat screen.

    // ---- command typer: '/' opens MC's command screen, type, Enter ----
    const bool screenOpen = mc && (mc->flags & proto::kMcScreenOpen);
    switch (g_typer.load())
    {
    case Typer::Idle: {
        if (!inWorld || screenOpen)
            break;
        std::lock_guard lk(g_mutex);
        if (g_commands.empty())
            break;
        g_typing = g_commands.front();
        out.push_back({proto::kInKey, kSdlSlash, 1, 0, 0});
        out.push_back({proto::kInKey, kSdlSlash, 0, 0, 0});
        g_typer = Typer::WaitScreen;
        g_typerMs = now;
        break;
    }
    case Typer::WaitScreen:
        if (screenOpen)
        {
            for (unsigned char ch : g_typing)
                out.push_back({proto::kInText, 0, ch, 0, 0});
            g_typer = Typer::Typed;
            g_typerMs = now;
        }
        else if (now - g_typerMs > kScreenTimeoutMs || !mcAlive)
        {
            WLOG("build mode: Minecraft's command screen didn't open for \"/%s\" (try %d)", g_typing.c_str(), g_typerTries + 1);
            g_typer = Typer::Idle;
            if (++g_typerTries >= 3)
            {
                std::lock_guard lk(g_mutex);
                if (!g_commands.empty())
                    g_commands.pop_front();
                g_typerTries = 0;
            }
        }
        break;
    case Typer::Typed:
        if (now - g_typerMs > 150)
        {
            out.push_back({proto::kInKey, kSdlReturn, 1, 0, 0});
            out.push_back({proto::kInKey, kSdlReturn, 0, 0, 0});
            g_typer = Typer::WaitClose;
            g_typerMs = now;
        }
        break;
    case Typer::WaitClose:
        if (!screenOpen || now - g_typerMs > kScreenTimeoutMs)
        {
            if (screenOpen)
            {
                WLOG("build mode: command screen still open after \"/%s\"; closing it", g_typing.c_str());
                out.push_back({proto::kInKey, 41, 1, 0, 0}); // Escape
                out.push_back({proto::kInKey, 41, 0, 0, 0});
            }
            else
            {
                WLOG("build mode: sent \"/%s\" to Minecraft", g_typing.c_str());
            }
            ++g_commandsDone;
            g_typerTries = 0;
            {
                std::lock_guard lk(g_mutex);
                if (!g_commands.empty())
                    g_commands.pop_front();
            }
            g_typer = Typer::Idle;
        }
        break;
    }

    // ---- flush: queued (tests, script) first, then this iteration's events ----
    {
        std::lock_guard lk(g_mutex);
        if (!g_events.empty())
        {
            out.insert(out.begin(), g_events.begin(), g_events.end());
            g_events.clear();
        }
    }
    if (!mcAlive)
        return; // MC isn't draining; queued events are dropped (they'd be stale)
    for (const auto& ev : out)
    {
        WriteRing(base, ev, g_dropped);
        ++g_eventsSent;
    }
}

std::string StatusSuffix()
{
    char buf[128];
    std::snprintf(buf, sizeof(buf), " | build=%s cmds=%zu sent=%llu", g_enabled.load() ? "ON" : "off", CommandsPending(),
                  static_cast<unsigned long long>(g_eventsSent));
    return buf;
}
} // namespace wc::buildmode
