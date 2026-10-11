# WitcherCraft

Play The Witcher 3 as Steve from Minecraft. You walk, build, mine and fight the Minecraft way, in
Geralt's world, with its people, monsters and quests.

WitcherCraft is based on [SkyCraft](https://github.com/chasmlol/SkyCraft), which does the same for Skyrim.

## What you can do

- Walk, sprint, jump, swim and fly around the Witcher's world. You bump into its ground, walls and
  buildings like you would in Minecraft.
- Place and break blocks anywhere.
- Fight the Witcher's enemies with Minecraft weapons.
- Bring Minecraft mobs into the fight: build an iron golem, or use spawn eggs in creative mode.
  Iron golems and your tamed wolves attack the Witcher's enemies and leave you alone.
  Zombies, skeletons, creepers and other hostile mobs attack the enemies too, but they go for
  whoever is closer, so keep your distance.
- Torches and other glowing blocks light up the world around them.
- Call a horse, use Witcher senses, and open the map, journal and inventory without leaving Steve.
- Turn back into Geralt whenever you want, and back into Steve again.

## What you need

- **Windows 10 or 11** (64-bit).
- **The Witcher 3: Wild Hunt - Remastered.** Tested on patches 5.0 and 5.01 (Steam).
- **Minecraft: Java Edition**, bought on a Microsoft account. You don't have to install the normal
  Minecraft Launcher, WitcherCraft brings its own. You only need the account that owns the game.
- **16 GB of RAM or more.** Both games run at the same time.
- **A keyboard and mouse.** Controllers don't work yet.
- **An internet connection** the first time you play, to sign in and download Minecraft.

## How to install

Take your time with these steps. You only do this once.

### Step 1: Download WitcherCraft

1. Go to the [Releases page](https://github.com/Wanesia/WitcherCraft/releases).
2. Under the newest version, click **WitcherCraft-0.1.0.zip** (the number may be higher) to download it.
   Download the `.zip` file, not the `.jar`.

### Step 2: Open your Witcher 3 folder

1. Open **Steam** and go to your **Library**.
2. Right-click **The Witcher 3: Wild Hunt**.
3. Click **Manage**, then **Browse local files**.

A folder opens. You're in the right place if you can see folders called **bin** and **content**.
Leave this window open.

### Step 3: Copy WitcherCraft into the game

1. Go to your **Downloads** folder and double-click **WitcherCraft-0.1.0.zip** to open it.
   Inside you'll see two folders: **bin** and **mods**.
2. Copy both folders into the Witcher 3 folder you opened in Step 2.
3. If Windows says you need administrator permission, click **Continue**.
4. If Windows asks whether to replace a file called **dinput8.dll**, click **Skip**: another mod
   already uses that file. Read [I already have a dinput8.dll](#i-already-have-a-dinput8dll) before you go on.

### Step 4: Set up Minecraft (only the first time)

1. **Start The Witcher 3** like you normally do.
2. Instead of the game, a **WitcherCraft** message appears. Click **OK**. The game closes, which is
   expected: Minecraft is set up first, without the game.
3. A window called **Prism Launcher** opens. This is the Minecraft launcher that comes with
   WitcherCraft. It asks you to add a Microsoft account. Add the account that owns Minecraft.
4. Prism downloads Minecraft. This takes a few minutes. When it's done, the Prism window closes by
   itself.
5. **Start The Witcher 3 again.** This time the game opens normally. Minecraft runs hidden in the
   background, so you won't see a Minecraft window. That's normal.

### Step 5: Change two game settings

1. In the game's main menu, go to **Options**, then **Video**, then **Graphics**.
2. Turn **Frame Generation** off (if your graphics card has it). Minecraft's blocks shake and smear
   with it on.
3. Turn **Motion Blur** off too. It isn't required, but blocks look much better without it.

That's the install done. Load a save: you'll play as Geralt until Minecraft is ready, then you turn into
Steve.

From now on, just start The Witcher 3. Minecraft starts and stops with it.

## How to play

When you load a save, you are Steve. These are WitcherCraft's keys:

| Key | What it does |
|---|---|
| **F8** | Switch between Steve and Geralt. Press it again to switch back. |
| **G** | Talk to people, loot, open doors, get on a horse. |
| **V** (hold) | Witcher senses. |
| **X** | Call Steve's horse. |
| **M / J / I / K** | The Witcher's map, journal, inventory and character screens. You stay Steve. |
| **N** | Meditate. You turn back into Steve afterwards. |
| **Esc** | Minecraft's menu. Click **WitcherCraft...** in it to change keys and options. |

**Saving your game:** Steve can't open the Witcher's save menu. Press **F8** to become Geralt, then
press **Esc** and save like normal. Minecraft saves your blocks and items by itself.

**One world for the whole game:** Steve's blocks are in a single Minecraft world. A house you build in
Velen also stands at the same spot in Skellige, Toussaint and every other region.

**Your blocks are Steve's only:** as Geralt you don't see them and walk straight through them, and the
Witcher's people and monsters walk through them too.

**Field of view:** Minecraft's FOV is locked at 70 while you play. The Witcher's camera can't go as wide
as Minecraft's FOV slider, and any other value makes your blocks look like they float and slide around
as you look.

**If something feels wrong**, press **F8** to get Geralt back. In the **WitcherCraft...** menu,
**Reset everything to defaults** undoes any settings you changed.

## Updating WitcherCraft

Download the new zip and copy its **bin** and **mods** folders into the game folder, the same way as in
[Step 3](#step-3-copy-witchercraft-into-the-game). When Windows asks, choose **Replace the files in the
destination**. You don't need to sign in again, and your Minecraft blocks and items are kept.

If you use the **witchercraft.asi** setup (see [I already have a dinput8.dll](#i-already-have-a-dinput8dll)),
don't let the new **dinput8.dll** replace your other mod's file. Click **Skip** for it, then take the new
**dinput8.dll** out of the zip, rename it to **witchercraft.asi** and replace your old witchercraft.asi
with it.

## Uninstalling

Open your Witcher 3 folder (see [Step 2](#step-2-open-your-witcher-3-folder)) and delete:

- inside `bin\x64_dx12`: **dinput8.dll** (or **witchercraft.asi** if you used that instead),
  **witchercraft_native.ini**, **witchercraft_native.log** and the **witchercraft** folder
- inside `mods`: the **modWitcherCraft** folder

That's it. Everything you built as Steve is kept, so if you install WitcherCraft again later, your
builds are still there. Your Witcher saves are never touched.

**To delete everything, including your builds:** after the steps above, press **Windows key + R**,
type `%LOCALAPPDATA%` and press **Enter**. Delete the **WitcherCraft** folder. This deletes Steve's
world (everything you built), your sign-in and the Minecraft that came with WitcherCraft. It can't be
undone.

## Using your own Minecraft launcher

You don't need this. WitcherCraft comes with its own launcher and sets everything up for you. This is
only for people who already play Minecraft through Prism Launcher or MultiMC and want WitcherCraft to
use that instead.

1. In your launcher, make an instance called `WitcherCraft` with **Minecraft 26.3**, **Fabric Loader
   0.19.5** and **Java 25**. Add **Fabric API** and **witchercraft-minecraft-0.1.0.jar** (from the Releases page)
   to its mods, and add `--enable-native-access=ALL-UNNAMED -Dwitchercraft.startHidden=true` to its JVM
   arguments.
2. Open `bin\x64_dx12\witchercraft_native.ini` in the game folder with Notepad. WitcherCraft creates
   this file the first time you start the game, so if it isn't there yet, start the game once.
3. After `Launcher=`, type the full path of your launcher, for example
   `Launcher=C:\Games\PrismLauncher\prismlauncher.exe`, and save.

WitcherCraft updates never change this file. To go back to the built-in launcher, delete the file.

Your builds stay with the launcher you made them in. Things you built with the built-in launcher won't
show up in your own launcher's instance, but they aren't deleted either: switch back and they're there.

## Problems and questions

### Nothing happens, I'm still Geralt

- Wait a minute after loading a save. Minecraft can take a while to get ready, especially the first time.
- Check [Step 3](#step-3-copy-witchercraft-into-the-game) again. A common mistake is ending up with an
  extra folder, like `The Witcher 3\WitcherCraft-0.1.0\bin\...`. The **bin** and **mods** folders must go
  straight into the Witcher 3 folder.

### The WitcherCraft message appears every time I start the game

Minecraft isn't set up yet. Click **OK**, then add your Microsoft account in the Prism Launcher window
and wait until the window closes by itself before you start the game again. If you closed Prism
before it finished, the message comes back so you can try again.

### Prism says my account doesn't own Minecraft

You need to buy **Minecraft: Java Edition** on that Microsoft account. If you have more than one
Microsoft account, make sure you signed in with the one you bought it on.

### Prism asks me to choose a Minecraft name

This happens if you've never played Minecraft: Java Edition on that account. Pick a name. You only do
it once.

### The game shows script errors from modWitcherCraft after a game update

The game update changed something WitcherCraft depends on. Uninstall WitcherCraft (see above) and wait
for a WitcherCraft update that supports the new game version. Your builds are kept and come back when
you install the update.

### I already have a dinput8.dll

Some other mods (usually an "ASI loader") use a file with the same name. Keep theirs. Take
**dinput8.dll** out of WitcherCraft's zip (it's in `bin\x64_dx12`), rename it to **witchercraft.asi**,
and put it in the game's `bin\x64_dx12` folder. Copy the rest of the zip as usual. This setup hasn't
been tested yet, so please report whether it works for you.

### Something else is wrong

Please open an issue on the [Issues page](https://github.com/Wanesia/WitcherCraft/issues) and describe
what happened. It helps a lot if you attach these two log files:

- `The Witcher 3\bin\x64_dx12\witchercraft_native.log`
- `%LOCALAPPDATA%\WitcherCraft\Prism\instances\WitcherCraft\.minecraft\logs\latest.log`

The logs contain your Windows user name (in folder paths) and your Minecraft name. If you don't want
those to be public, open the files in Notepad and replace them with something else before you attach
them.

## Build from source

Tools: Visual Studio 2022 Build Tools (C++), CMake 3.21+, JDK 25, Python 3 (for the helper scripts).
Clone to a short path (for example `C:\src\WitcherCraft`): the Visual Studio build fails in folders
whose paths get longer than Windows' 260-character limit.

```powershell
# Native plugin: native\build\Release\dinput8.dll (also witchercraft.asi) + test tools
powershell -ExecutionPolicy Bypass -File native\build.ps1

# Install into the game (refuses to overwrite a different dinput8.dll); -Game "<path>" for non-default paths
powershell -ExecutionPolicy Bypass -File install.ps1

# Start the hidden Minecraft (dev client), then start The Witcher 3
.\"Start Minecraft (WitcherCraft).cmd"
```

`uninstall.ps1` removes everything `install.ps1` added. Saves are not touched. A source install doesn't
bundle Minecraft, so the plugin doesn't start one; use the `.cmd` above.

## Project layout

| Path | What it is |
|---|---|
| `native/src/` | The C++ plugin loaded by The Witcher 3 (as `dinput8.dll`): hooks, shared-memory host, input, camera, DX12 overlay, collision, combat. |
| `native/tools/` | Offline tests and helpers. `fake_host.exe <mode>` plays the Witcher's side against a real hidden Minecraft without the game (modes: `control`, `identity`, `combat`, `features`, `flight`, `walls`, `tnt`, `mobs`, `interact`, `idle`); `restart_mc.ps1` restarts that Minecraft; `wcraft_scan` / `col_scan` check a new game patch; `ws_symcheck.py` checks script names against the game's scripts. |
| `mod/modWitcherCraft/` | WitcherScript files that run inside the game (HUD, blocks, combat, controls, lights). |
| `minecraft/` | The Fabric mod (Minecraft 26.3, Mojang names). |
| `protocol/` | The shared-memory layout both sides use. |
| `render/tools/` | Python helpers for reading the Witcher's `.bundle` archives. |
| `release/` | `package.ps1`, the Prism Launcher instance template (`minecraft-bundle/`) and the players' `README.txt`. |

## License

MIT, see `LICENSE`. Credits and third-party licenses are in `THIRD-PARTY-NOTICES.md`.
WitcherCraft isn't affiliated with or endorsed by Mojang, Microsoft or CD PROJEKT RED.
