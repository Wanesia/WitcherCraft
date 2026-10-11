package dev.witchercraft;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.NoSuchFileException;
import java.nio.file.Path;
import java.util.Properties;
import net.fabricmc.loader.api.FabricLoader;

/**
 * WitcherCraft options (Esc menu -> "WitcherCraft..." , config/witchercraft.properties). Minecraft
 * applies its own (speed, hunger, climbing help, name tags); the Witcher's (HUD pieces, key hints,
 * interact key, occlusion) go to the host through McState (wcFlags / wcKeys).
 */
public final class WcOptions {
	/** DirectInput key codes (the Witcher side's input hook); KEY_NONE = unbound, KEY_MOUSE = a mouse button. */
	public static final int DIK_G = 0x22, DIK_F8 = 0x42, KEY_NONE = 0xFF, KEY_MOUSE = 0xFE;
	// The Witcher's HUD while playing as Steve
	public static volatile boolean minimap = true;
	public static volatile boolean questTracker = true;
	public static volatile boolean geraltHud = false;      // Geralt's health / medallion / buffs / quick slots
	public static volatile boolean witcherEnemyInfo = true; // the Witcher's enemy names and health bars
	public static volatile boolean mcBossBar = true;       // bosses: Minecraft's boss bar instead of the Witcher's
	// Minecraft
	public static volatile boolean mcNameTags = false;     // Minecraft name tags on Witcher characters
	public static volatile int speedPct = 100;             // Steve's walking/sprinting speed (Minecraft's own; up to 175 in Options)
	public static volatile boolean hunger = false;         // Minecraft hunger (off: never too hungry to sprint)
	public static volatile boolean climbHelp = false;      // jump onto ledges up to ~3 blocks
	// Both
	public static volatile boolean keyHints = true;        // the key hints when switching modes
	// Always on: walls hide blocks, fire, water putting it out.
	public static final boolean occlusion = true;          // Witcher walls hide Minecraft blocks behind them
	public static volatile boolean blockLights = true;     // torches etc. light the Witcher's world (WcLights)
	public static final boolean fire = true;               // Minecraft fire sets Witcher enemies burning
	public static final boolean waterDouse = true;         // Minecraft water puts burning Witcher enemies out
	public static volatile boolean panelKeys = true;       // M/J/I/K/N in Minecraft mode: switch to Geralt and open that panel
	public static volatile boolean offKeyTips = true;      // a toast when a key's WitcherCraft feature is turned off (WcTips)
	// Keys the Witcher side handles (DirectInput codes; KEY_NONE = unbound). Witcher senses and Call
	// Roach are Minecraft key bindings (WcKeys).
	public static volatile int interactKey = DIK_G;        // talk / loot / open / mount (G)
	public static volatile int modeKey = DIK_F8;           // switch between Steve and Geralt (F8; can't be unbound)
	// Minecraft's damage tilt was set to 0 once (blocks swung against the Witcher world on every hit);
	// after that it is the player's own setting again (SkyClient.lockFov).
	public static volatile boolean tiltDefaulted = false;

	public static final int[] SPEEDS = {100, 125, 150, 175};

	private static volatile int version;

	private WcOptions() {
	}

	/** Bumped on every change (the server re-applies speed / name tags). */
	public static int version() {
		return version;
	}

	private static Path file() {
		return FabricLoader.getInstance().getConfigDir().resolve("witchercraft.properties");
	}

	public static void load() {
		Properties p = new Properties();
		try (var in = Files.newBufferedReader(file())) {
			p.load(in);
		} catch (NoSuchFileException e) {
			return;
		} catch (IOException e) {
			WitcherCraft.LOG.warn("WitcherCraft: couldn't read {}", file(), e);
			return;
		}
		minimap = bool(p, "minimap", minimap);
		questTracker = bool(p, "questTracker", questTracker);
		geraltHud = bool(p, "geraltHud", geraltHud);
		witcherEnemyInfo = bool(p, "witcherEnemyInfo", witcherEnemyInfo);
		mcBossBar = bool(p, "mcBossBar", mcBossBar);
		blockLights = bool(p, "blockLights", blockLights);
		mcNameTags = bool(p, "mcNameTags", mcNameTags);
		speedPct = integer(p, "speedPct", speedPct);
		if (java.util.Arrays.stream(SPEEDS).noneMatch(v -> v == speedPct)) {
			speedPct = 100; // a hand-edited value the Options screen doesn't offer
		}
		hunger = bool(p, "hunger", hunger);
		climbHelp = bool(p, "climbHelp", climbHelp);
		keyHints = bool(p, "keyHints", keyHints);
		panelKeys = bool(p, "panelKeys", panelKeys);
		offKeyTips = bool(p, "offKeyTips", offKeyTips);
		interactKey = integer(p, "interactKey", interactKey);
		modeKey = integer(p, "modeKey", modeKey);
		tiltDefaulted = bool(p, "tiltDefaulted", tiltDefaulted);
		if (modeKey <= 0 || modeKey >= KEY_MOUSE) {
			modeKey = DIK_F8;
		}
		version++;
	}

	public static void save() {
		version++;
		Properties p = new Properties();
		p.setProperty("minimap", Boolean.toString(minimap));
		p.setProperty("questTracker", Boolean.toString(questTracker));
		p.setProperty("geraltHud", Boolean.toString(geraltHud));
		p.setProperty("witcherEnemyInfo", Boolean.toString(witcherEnemyInfo));
		p.setProperty("mcBossBar", Boolean.toString(mcBossBar));
		p.setProperty("blockLights", Boolean.toString(blockLights));
		p.setProperty("mcNameTags", Boolean.toString(mcNameTags));
		p.setProperty("speedPct", Integer.toString(speedPct));
		p.setProperty("hunger", Boolean.toString(hunger));
		p.setProperty("climbHelp", Boolean.toString(climbHelp));
		p.setProperty("keyHints", Boolean.toString(keyHints));
		p.setProperty("panelKeys", Boolean.toString(panelKeys));
		p.setProperty("offKeyTips", Boolean.toString(offKeyTips));
		p.setProperty("interactKey", Integer.toString(interactKey));
		p.setProperty("modeKey", Integer.toString(modeKey));
		p.setProperty("tiltDefaulted", Boolean.toString(tiltDefaulted));
		try (var out = Files.newBufferedWriter(file())) {
			p.store(out, "WitcherCraft options (Esc menu -> WitcherCraft...)");
		} catch (IOException e) {
			WitcherCraft.LOG.warn("WitcherCraft: couldn't write {}", file(), e);
		}
	}

	/** Every WitcherCraft option back to how it was installed (the Minecraft key bindings: WcKeys). */
	public static void resetDefaults() {
		minimap = true;
		questTracker = true;
		geraltHud = false;
		witcherEnemyInfo = true;
		mcBossBar = true;
		mcNameTags = false;
		speedPct = 100;
		hunger = false;
		climbHelp = false;
		keyHints = true;
		blockLights = true;
		panelKeys = true;
		offKeyTips = true;
		interactKey = DIK_G;
		modeKey = DIK_F8;
		save();
	}

	/**
	 * Everything WitcherCraft adds on top of plain "Minecraft in the Witcher" off: the Witcher's
	 * HUD pieces, climbing help, torch light, boss bars, name tags, key hints, the panel keys,
	 * normal Minecraft speed and hunger. Steve still walks the Witcher's world and fights its enemies;
	 * the Steve / Geralt switch stays (it is the only way back to Minecraft).
	 */
	public static void allExtrasOff() {
		minimap = false;
		questTracker = false;
		geraltHud = false;
		witcherEnemyInfo = false;
		mcBossBar = false;
		mcNameTags = false;
		speedPct = 100;
		hunger = true;
		climbHelp = false;
		keyHints = false;
		blockLights = false;
		panelKeys = false;
		// offKeyTips stays: with everything off, a tip is how a player learns why M does nothing.
		save();
	}

	private static boolean bool(Properties p, String key, boolean def) {
		String v = p.getProperty(key);
		return v == null ? def : Boolean.parseBoolean(v.trim());
	}

	private static int integer(Properties p, String key, int def) {
		try {
			String v = p.getProperty(key);
			return v == null ? def : Integer.parseInt(v.trim());
		} catch (NumberFormatException e) {
			return def;
		}
	}

	// ---- McState wcFlags (host side: protocol kWc*) ----
	public static final int WC_VALID = 1, WC_SENSES = 1 << 1, WC_MINIMAP = 1 << 2, WC_QUESTS = 1 << 3, WC_GERALT_HUD = 1 << 4, WC_ENEMY_INFO = 1 << 5,
		WC_KEY_HINTS = 1 << 6, WC_OCCLUSION = 1 << 7, WC_MC_BOSSBAR = 1 << 8, WC_PANEL_KEYS = 1 << 9;

	public static int flags(boolean sensesHeld) {
		int f = WC_VALID;
		f |= sensesHeld ? WC_SENSES : 0;
		f |= minimap ? WC_MINIMAP : 0;
		f |= questTracker ? WC_QUESTS : 0;
		f |= geraltHud ? WC_GERALT_HUD : 0;
		f |= witcherEnemyInfo ? WC_ENEMY_INFO : 0;
		f |= keyHints ? WC_KEY_HINTS : 0;
		f |= occlusion ? WC_OCCLUSION : 0;
		f |= mcBossBar ? WC_MC_BOSSBAR : 0;
		f |= panelKeys ? WC_PANEL_KEYS : 0;
		return f;
	}

	/** McState wcKeys: interact, Steve/Geralt switch, Witcher senses, Call Roach (one DIK per byte). */
	public static int keys(int sensesDik, int roachDik) {
		return (interactKey & 0xFF) | (modeKey & 0xFF) << 8 | (sensesDik & 0xFF) << 16 | (roachDik & 0xFF) << 24;
	}
}
