package dev.witchercraft.client;

import dev.witchercraft.WitcherCraft;
import java.util.List;
import java.util.function.Function;
import net.minecraft.client.Minecraft;
import net.minecraft.client.Screenshot;
import net.minecraft.client.gui.screens.PauseScreen;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.client.gui.screens.options.OptionsScreen;

/**
 * Offline check of the WitcherCraft screens (only with the environment variable
 * WITCHERCRAFT_SCREEN_TOUR=1): once in the world, opens the pause menu and each WitcherCraft screen
 * in turn, then Minecraft's own Options screen (FOV slider locked), and saves a screenshot of each
 * (run\screenshots\wc_tour_*.png; "_end" = scrolled to the bottom), then closes them.
 */
final class WcScreenTour {
	private record Stop(String name, Function<Screen, Screen> screen, boolean scrolled) {
	}

	private static final boolean ENABLED = System.getenv("WITCHERCRAFT_SCREEN_TOUR") != null;
	private static final List<Stop> STOPS = List.of(new Stop("pause", p -> new PauseScreen(true), false), new Stop("menu", WcMenuScreen::new, false),
		new Stop("about", WcAboutScreen::new, false), new Stop("about_end", WcAboutScreen::new, true), new Stop("options", WcOptionsScreen::new, false),
		new Stop("options_end", WcOptionsScreen::new, true), new Stop("controls", WcControlsScreen::new, false),
		new Stop("controls_end", WcControlsScreen::new, true),
		new Stop("mc_options", p -> new OptionsScreen(p, Minecraft.getInstance().options), false));
	private static int ticks = -200; // wait ~10 s in the world first
	private static int step = 0;

	private WcScreenTour() {
	}

	static void tick(Minecraft minecraft) {
		if (!ENABLED || step > STOPS.size() || minecraft.player == null || !SkyClient.tookOver()) {
			return;
		}
		ticks++;
		if (ticks == 0) {
			if (step == STOPS.size()) {
				minecraft.gui.setScreen(null);
				WitcherCraft.LOG.info("WitcherCraft: screen tour done");
				step++;
				return;
			}
			minecraft.gui.setScreen(STOPS.get(step).screen().apply(new PauseScreen(true)));
		} else if (ticks == 10 && STOPS.get(step).scrolled() && minecraft.gui.screen() != null) {
			Screen s = minecraft.gui.screen();
			s.mouseScrolled(s.width / 2.0, s.height / 2.0, 0.0, -100.0);
		} else if (ticks == 30) {
			String name = "wc_tour_" + STOPS.get(step).name() + ".png";
			Screenshot.grab(minecraft.gameDirectory, name, minecraft.gameRenderer.mainRenderTarget(), 1,
				msg -> WitcherCraft.LOG.info("WitcherCraft: screen tour {}: {}", name, msg.getString()));
			step++;
			ticks = -10;
		}
	}
}
