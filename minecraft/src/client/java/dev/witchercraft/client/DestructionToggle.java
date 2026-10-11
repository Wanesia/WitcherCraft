package dev.witchercraft.client;

import dev.witchercraft.WcOptions;
import dev.witchercraft.WitcherCraft;
import dev.witchercraft.world.SkyDig;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.NoSuchFileException;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Properties;
import net.fabricmc.fabric.api.client.screen.v1.ScreenEvents;
import net.fabricmc.fabric.api.client.screen.v1.Screens;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.components.Button;
import net.minecraft.client.gui.components.Tooltip;
import net.minecraft.client.gui.screens.PauseScreen;
import net.minecraft.network.chat.CommonComponents;
import net.minecraft.network.chat.Component;

/**
 * The pause menu's "World destruction" button (its top left corner, clear of the menu at any GUI
 * scale) and its setting, {@code destruction=} in config/witchercraft-world.properties (see SkyDig.destruction).
 */
public final class DestructionToggle {
	private static final String KEY = "destruction";

	private DestructionToggle() {
	}

	private static Path file() {
		return FabricLoader.getInstance().getConfigDir().resolve("witchercraft-world.properties");
	}

	public static void register() {
		load();
		ScreenEvents.AFTER_INIT.register((minecraft, screen, width, height) -> {
			if (screen instanceof PauseScreen pause && pause.showsPauseMenu() && minecraft.player != null) {
				Screens.getWidgets(screen).add(button(minecraft));
				if (SkyClient.tookOver()) {
					lockQuit(screen);
					// WitcherCraft: about, options (HUD pieces, fire, speed, ...), controls, reset / all off.
					Screens.getWidgets(screen).add(Button.builder(Component.literal("WitcherCraft..."),
						b -> minecraft.gui.setScreen(new WcMenuScreen(screen))).bounds(4, 28, 150, 20)
						.tooltip(Tooltip.create(Component.literal("About the mod, its options and its keys."))).build());
				}
			}
		});
	}

	/**
	 * WitcherCraft: while the game drives this Minecraft, "Save and Quit to Title" / "Disconnect"
	 * would drop the world under the player for the rest of the session (nothing rejoins it).
	 * It becomes "Save and Quit to The Witcher": leave Minecraft mode, as F8 does. Minecraft saves
	 * now, and saves and closes by itself when the game closes.
	 */
	private static void lockQuit(net.minecraft.client.gui.screens.Screen screen) {
		Component local = CommonComponents.disconnectButtonLabel(true), remote = CommonComponents.disconnectButtonLabel(false);
		var widgets = Screens.getWidgets(screen);
		for (int i = 0; i < widgets.size(); i++) {
			if (widgets.get(i) instanceof Button b && (b.getMessage().equals(local) || b.getMessage().equals(remote))) {
				Button quit = Button.builder(Component.literal("Save and Quit to The Witcher"), btn -> {
					Minecraft minecraft = Minecraft.getInstance();
					var server = minecraft.getSingleplayerServer();
					if (server != null) {
						server.execute(() -> server.getCommands().performPrefixedCommand(server.createCommandSourceStack().withSuppressedOutput(), "save-all"));
					}
					dev.witchercraft.link.SkyLink.pushEvent(dev.witchercraft.link.Proto.EV_LEAVE_MC_MODE, 0, 0, 0, 0, 0, 0);
					minecraft.gui.setScreen(null);
					WitcherCraft.LOG.info("WitcherCraft: pause menu: saved, back to The Witcher (Minecraft mode off)");
				}).bounds(b.getX(), b.getY(), b.getWidth(), b.getHeight())
					.tooltip(Tooltip.create(Component.literal("Back to Geralt, like ").append(WcKeyCodes.name(WcOptions.modeKey))
						.append(". Minecraft closes by itself when you quit The Witcher 3.")))
					.build();
				widgets.set(i, quit);
				WitcherCraft.LOG.info("WitcherCraft: pause menu '{}' -> 'Save and Quit to The Witcher'", b.getMessage().getString());
			}
		}
	}

	private static Button button(Minecraft minecraft) {
		// The host's server does all the digging, so in a friend's world it's their setting.
		boolean host = minecraft.hasSingleplayerServer();
		Button button = Button.builder(label(), b -> {
			SkyDig.destruction = !SkyDig.destruction;
			b.setMessage(label());
			save();
			WitcherCraft.LOG.info("WitcherCraft: world destruction {}", SkyDig.destruction ? "on" : "off");
		}).bounds(4, 4, 150, 20).tooltip(Tooltip.create(Component.literal(host
			? "Mining The Witcher's ground and rocks, and explosions, dig into its world. Holes already dug stay either way."
			: "In a friend's world, their setting decides."))).build();
		button.active = host;
		return button;
	}

	/** "Reset everything to defaults" (WcMenuScreen): world destruction back on, as installed. */
	public static void resetDefault() {
		SkyDig.destruction = true;
		save();
	}

	private static Component label() {
		return Component.literal("World destruction: " + (SkyDig.destruction ? "On" : "Off"));
	}

	private static void load() {
		Properties props = new Properties();
		try (var in = Files.newBufferedReader(file())) {
			props.load(in);
		} catch (NoSuchFileException e) {
			return;
		} catch (IOException e) {
			WitcherCraft.LOG.warn("WitcherCraft: couldn't read {}", file(), e);
			return;
		}
		SkyDig.destruction = !"false".equalsIgnoreCase(props.getProperty(KEY, "true").trim());
	}

	/** Rewrites only its own line, keeping the file's comments and other settings (join=). */
	private static void save() {
		Path file = file();
		String line = KEY + "=" + SkyDig.destruction;
		try {
			List<String> lines = Files.exists(file) ? new ArrayList<>(Files.readAllLines(file)) : new ArrayList<>(List.of("# WitcherCraft"));
			boolean found = false;
			for (int i = 0; i < lines.size(); i++) {
				if (lines.get(i).trim().startsWith(KEY + "=")) {
					lines.set(i, line);
					found = true;
				}
			}
			if (!found) {
				lines.add("# Mining and explosions dig into the Witcher's world (the pause menu's \"World destruction\" button).");
				lines.add(line);
			}
			Files.createDirectories(file.getParent());
			Files.write(file, lines);
		} catch (IOException e) {
			WitcherCraft.LOG.warn("WitcherCraft: couldn't save {}", file, e);
		}
	}
}
