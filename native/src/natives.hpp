#pragma once
#include <string>

namespace wc::natives
{
// Installs the registration hook (call site inside the engine's math-native export).
bool InstallRegistrationHook();
// Text command handler shared by WCraft_Rpc and the Log() fallback channel.
std::string HandleRpc(const std::string& request);
// M2b block / build-mode natives (natives_blocks.cpp). Returns how many registered.
int RegisterBlockNatives();
// M3a Minecraft-mode natives (natives_control.cpp). Returns how many registered.
int RegisterControlNatives();
// M4a combat natives (natives_combat.cpp). Returns how many registered.
int RegisterCombatNatives();
// M5a W3 wall/building collision natives (natives_collision.cpp). Returns how many registered.
int RegisterCollisionNatives();
// M5b occlusion natives (natives_visual.cpp). Returns how many registered.
int RegisterVisualNatives();
} // namespace wc::natives
