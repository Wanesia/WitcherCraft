# Third-party notices

WitcherCraft is MIT-licensed (see `LICENSE`).

## SkyCraft

WitcherCraft is a port of [SkyCraft](https://github.com/chasmlol/SkyCraft) by chasmlol (MIT).
The Minecraft mod in `minecraft/` started as SkyCraft's Fabric mod, the shared-memory layout in
`protocol/skycraft_protocol.h` is SkyCraft's, and parts of the native host (`native/src/skycraft_host.*`,
`native/src/combat.cpp`, `native/src/launcher.cpp`) are ported from SkyCraft's Skyrim plugin.
SkyCraft's license text is in `minecraft/LICENSE-SkyCraft.txt` (and inside the WitcherCraft jar).

## Minecraft-Ring

Minecraft mobs fighting the Witcher's enemies (`minecraft/src/main/java/dev/witchercraft/combat/WitcherEnemyTargetGoal.java`)
is adapted from the bridge code's `ErEnemyTargetGoal` in [Minecraft-Ring](https://github.com/siddoff/Minecraft-Ring)
(bridge code by justbustin, MIT). Its license text is in `minecraft/LICENSE-MinecraftRing.txt` (and inside the WitcherCraft jar).

## In the bundled Minecraft (`WitcherCraft-Minecraft.zip` in a release)

| Component | License | Source |
|---|---|---|
| Prism Launcher (unmodified portable Windows build) | GPL-3.0 | https://github.com/PrismLauncher/PrismLauncher (the version is in the bundle's `Prism/THIRD-PARTY.txt`) |
| Fabric API | Apache-2.0 | https://github.com/FabricMC/fabric |

The bundle carries Prism Launcher's full license text as `Prism/LICENSE-PrismLauncher.txt`.

## Built against, not included

| Component | License | Source |
|---|---|---|
| Fabric Loader | Apache-2.0 | https://github.com/FabricMC/fabric-loader |
| Fabric Loom (build only) | MIT | https://github.com/FabricMC/fabric-loom |

## Not included

Minecraft, Java, Fabric Loader and The Witcher 3 are not included. Prism Launcher downloads
Minecraft, Java and Fabric Loader from Mojang, the Java vendor and FabricMC after the player signs
in with a Microsoft account that owns Minecraft: Java Edition.

WitcherCraft isn't affiliated with or endorsed by Mojang, Microsoft or CD PROJEKT RED.
