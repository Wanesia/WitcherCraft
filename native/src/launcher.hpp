#pragma once
#include <Windows.h>

// Starts Minecraft together with The Witcher 3 (ported from SkyCraft's skse/src/Launcher.cpp, MIT).
//
// A release puts the bundled Minecraft next to the plugin: bin\x64_dx12\witchercraft\WitcherCraft-Minecraft.zip
// (a portable Prism Launcher with a ready "WitcherCraft" instance: Minecraft, Fabric, Fabric API, our mod).
// It is unpacked to %LOCALAPPDATA%\WitcherCraft the first time and again whenever a new release brings a
// different bundle (Prism's sign-in, the downloaded Minecraft/Java and the world are kept), then started.
// The first time, Prism asks the player to sign in with the Microsoft account that owns Minecraft.
//
// witchercraft_native.ini, section [Minecraft] (a release install writes the file with these defaults the
// first time; the zip doesn't carry it, so updates keep the player's copy):
//   StartWithWitcher=1        ; 0 = start Minecraft yourself
//   Launcher=                 ; empty: the bundled Minecraft. Or a full path to your own launcher (Prism,
//                             ; MultiMC, or a .bat/.cmd file) to use that instead
//   Arguments=--launch WitcherCraft ; what to pass to your own launcher
//
// Without a bundle and without Launcher= (a source/dev install) nothing is started: run
// "Start Minecraft (WitcherCraft).cmd" as before.
namespace wc::launcher
{
// First start of a release install (the bundled Prism has no signed-in account yet): asks the player to
// sign in and let Prism download Minecraft without the game, then starts Prism. True = the game should
// close now (it's called from DllMain, before any of the game's own code, so nothing of the game has
// loaded). Loading the game while Prism signs in and downloads crashed it (a graphics driver reset).
bool FirstStartSetup(HMODULE self);

// Reads the settings and, if wanted, starts Minecraft on a background thread. Call once at start-up
// (safe from DllMain: it only creates a thread, which runs after the loader lock is released).
void Start(HMODULE self);
} // namespace wc::launcher
