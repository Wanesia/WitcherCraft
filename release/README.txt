WitcherCraft
============

Play The Witcher 3 as Steve from Minecraft. You walk, build, mine and fight the Minecraft way, in
Geralt's world, with its people, monsters and quests.


WHAT YOU NEED
-------------
- Windows 10 or 11 (64-bit).
- The Witcher 3: Wild Hunt - Remastered. Tested on patches 5.0 and 5.01 (Steam).
- Minecraft: Java Edition, bought on a Microsoft account. You don't have to install the normal
  Minecraft Launcher, WitcherCraft brings its own. You only need the account that owns the game.
- 16 GB of RAM or more. Both games run at the same time.
- A keyboard and mouse. Controllers don't work yet.
- An internet connection the first time you play.


HOW TO INSTALL
--------------
Step 1: Open your Witcher 3 folder
  1. Open Steam and go to your Library.
  2. Right-click The Witcher 3: Wild Hunt, click Manage, then Browse local files.
  3. A folder opens. You're in the right place if you can see the folders "bin" and "content".

Step 2: Copy WitcherCraft into the game
  1. Double-click the WitcherCraft zip to open it. Inside are two folders: "bin" and "mods".
  2. Copy both folders into the Witcher 3 folder you opened in Step 1.
  3. If Windows says you need administrator permission, click Continue.
  4. If Windows asks whether to replace "dinput8.dll", click Skip and read
     "I already have a dinput8.dll" below.

  The bin and mods folders must go straight into the Witcher 3 folder, not into a new folder
  inside it.

Step 3: Set up Minecraft (only the first time)
  1. Start The Witcher 3 like you normally do.
  2. Instead of the game, a WitcherCraft message appears. Click OK. The game closes, which is
     expected: Minecraft is set up first, without the game.
  3. A window called "Prism Launcher" opens. This is the Minecraft launcher that comes with
     WitcherCraft. It asks you to add a Microsoft account. Add the account that owns Minecraft.
  4. Prism downloads Minecraft. This takes a few minutes. Then the Prism window closes by itself.
  5. Start The Witcher 3 again. This time the game opens normally. Minecraft runs hidden, so you
     won't see a Minecraft window.

Step 4: Change two game settings
  1. In the game's main menu, go to Options, then Video, then Graphics.
  2. Turn Frame Generation off (if you have it). Blocks shake and smear with it on.
  3. Turn Motion Blur off too. Not required, but blocks look much better.

That's the install done. Load a save: you play as Geralt until Minecraft is ready, then you
turn into Steve.

From now on, just start The Witcher 3. Minecraft starts and stops with it.


HOW TO PLAY
-----------
When you load a save, you are Steve. These are WitcherCraft's keys:
  F8          Switch between Steve and Geralt.
  G           Talk, loot, open doors, get on a horse.
  V (hold)    Witcher senses.
  X           Call Steve's horse.
  M J I K     The Witcher's map, journal, inventory and character screens (you stay Steve).
  N           Meditate. You turn back into Steve afterwards.
  Esc         Minecraft's menu. Click "WitcherCraft..." to change keys and options.

Saving: press F8 to become Geralt, then Esc, and save like normal. Minecraft saves your blocks
and items by itself.

One world for the whole game: Steve's blocks are in a single Minecraft world. A house you build
in Velen also stands at the same spot in Skellige, Toussaint and every other region.

Your blocks are Steve's only: as Geralt you don't see them and walk straight through them, and
the Witcher's people and monsters walk through them too.

Field of view: Minecraft's FOV is locked at 70 while you play. The Witcher's camera can't go as
wide as Minecraft's FOV slider, and any other value makes your blocks look like they float and
slide around as you look.

If something feels wrong, press F8 to get Geralt back.


UPDATING
--------
Copy the new zip's bin and mods folders into the game folder like before, and choose
"Replace the files in the destination". Your sign-in and Steve's world are kept.

If you use the witchercraft.asi setup (see "I already have a dinput8.dll" below), click Skip for
dinput8.dll so your other mod's file stays. Then take the new dinput8.dll out of the zip, rename
it to witchercraft.asi and replace your old witchercraft.asi with it.


UNINSTALLING
------------
In the Witcher 3 folder, delete these from bin\x64_dx12: dinput8.dll (or witchercraft.asi if you
used that instead), witchercraft_native.ini, witchercraft_native.log and the witchercraft folder.
Then delete the folder mods\modWitcherCraft.

That's it. Everything you built as Steve is kept, so if you install WitcherCraft again later,
your builds are still there. Your Witcher saves are never touched.

To delete everything, including your builds: after the step above, press Windows key + R, type
%LOCALAPPDATA% and press Enter. Delete the "WitcherCraft" folder. This deletes Steve's world
(everything you built), your sign-in and the Minecraft that came with WitcherCraft. It can't be
undone.


PROBLEMS
--------
Nothing happens, I'm still Geralt
  - Wait a minute after loading a save. Minecraft can take a while to get ready.
  - Check that bin and mods went straight into the Witcher 3 folder.

The WitcherCraft message appears every time I start the game
  - Minecraft isn't set up yet. Click OK, add your Microsoft account in the Prism Launcher window
    and wait until it closes by itself before starting the game again.

Prism says my account doesn't own Minecraft
  - Buy Minecraft: Java Edition on that account, or sign in with the account you bought it on.

Prism asks me to choose a Minecraft name
  - You've never played Java Edition on that account. Pick a name, you only do it once.

Script errors from modWitcherCraft after a game update
  - The update changed something WitcherCraft depends on. Uninstall WitcherCraft and wait for a
    WitcherCraft update. Your builds are kept and come back when you install the update.

I already have a dinput8.dll
  - Another mod (usually an "ASI loader") uses that name. Keep theirs. Take dinput8.dll out of the
    WitcherCraft zip, rename it to witchercraft.asi and put it in bin\x64_dx12. Copy the rest as
    usual. Not tested yet.

Something else
  - Report it at https://github.com/Wanesia/WitcherCraft/issues and attach these logs:
    The Witcher 3\bin\x64_dx12\witchercraft_native.log
    %LOCALAPPDATA%\WitcherCraft\Prism\instances\WitcherCraft\.minecraft\logs\latest.log
  - The logs contain your Windows user name (in folder paths) and your Minecraft name. If you
    don't want those to be public, open the files in Notepad and replace them with something
    else before you attach them.


WitcherCraft is MIT-licensed and based on SkyCraft by chasmlol. Licences of the bundled parts are in
THIRD-PARTY-NOTICES.md. Not affiliated with or endorsed by Mojang, Microsoft or CD PROJEKT RED.
