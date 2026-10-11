#pragma once
// WitcherCraft M2b build mode: Witcher 3 input -> Minecraft (input ring), aiming, and the chat
// commands that give the Minecraft player blocks.
//
// Only the host thread writes the input ring (single producer). Other threads queue.
#include <cstdint>
#include <string>

namespace skycraft::proto
{
struct McState;
struct InputEvent;
}

namespace wc::buildmode
{
constexpr int kToggleVk = 0x76; // VK_F7: unused by W3 next-gen default bindings

// Any thread.
bool Enabled();
void SetEnabled(bool on);
void Toggle();
// Raw input event for Minecraft (proto::InputType, code, a). Sent by the host thread in order.
void QueueInput(uint16_t type, uint16_t code, int32_t a);
// Same, with every field (kInCursor needs b). Used by control.cpp (M3a) from the input hook.
void QueueEvent(const skycraft::proto::InputEvent& ev);
// A Minecraft chat command without the leading '/', typed into MC's command screen.
void QueueCommand(const std::string& command);
size_t CommandsPending();
// Gives the MC player its starter kit again (creative + hotbar) on the next build-mode tick.
void ResendKit();

// Script thread: where the player aims. W3 coords: camera position, camera direction (any
// length) and the W3 world hit (StaticTrace) with hitValid. Also traces the shown MC blocks.
// active=false stops aiming (MC's look follows the camera again). Returns the aim point in W3
// coords (w = 1) or w = 0 if nothing was hit within range.
struct Vec4
{
    float x, y, z, w;
};
Vec4 SetAim(bool active, const float camPos[3], const float camDir[3], const float hit[3], bool hitValid);

// Host thread: replace yaw/pitch with "MC eye -> aim point" while aiming. mc may be null.
// feet = the host player's feet in MC coords (fallback eye = feet + 1.62).
bool AimLook(const skycraft::proto::McState* mc, double feetX, double feetY, double feetZ, float& yaw, float& pitch);

// Host thread, every iteration. Polls the keyboard/mouse (only when this process owns the
// foreground window and the script is ticking), runs the command typer and flushes queued
// events into the input ring at `base`.
void HostPump(uint8_t* base, bool mcAlive, const skycraft::proto::McState* mc, uint32_t mcPid, bool scriptTicking);

std::string StatusSuffix();
} // namespace wc::buildmode
