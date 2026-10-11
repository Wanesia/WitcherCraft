package dev.witchercraft.client;

import dev.witchercraft.WitcherCraft;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientTickEvents;
import net.minecraft.client.Minecraft;
import net.minecraft.client.Screenshot;
import net.minecraft.client.gui.components.AbstractWidget;
import net.minecraft.client.gui.components.events.GuiEventListener;
import net.minecraft.client.gui.screens.PauseScreen;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.client.gui.screens.inventory.InventoryScreen;

/**
 * Offline check of the drawn cursor (only with the environment variable WITCHERCRAFT_CURSOR_CHECK=1,
 * linked to a host such as `fake_host idle`): once in the world, opens the inventory and points the
 * mouse at the first hotbar slot, then the pause menu pointing at a button, the way the host's
 * kInCursor events move it, and saves run\screenshots\wc_cursor_*.png. The cursor's tip must sit on
 * the highlighted slot / button.
 */
final class WcCursorCheck {
	private static final boolean ENABLED = System.getenv("WITCHERCRAFT_CURSOR_CHECK") != null;
	private static boolean registered;
	private static int ticks = -200; // ~10 s in the world first
	private static int step;

	private WcCursorCheck() {
	}

	static void register() {
		if (ENABLED && !registered) {
			registered = true;
			ClientTickEvents.END_CLIENT_TICK.register(WcCursorCheck::tick);
		}
	}

	private static void tick(Minecraft minecraft) {
		if (step > 2 || minecraft.player == null || !SkyClient.tookOver()) {
			return;
		}
		ticks++;
		if (step == 2) {
			if (ticks == 0) {
				minecraft.gui.setScreen(null);
				WitcherCraft.LOG.info("WitcherCraft: cursor check done");
				step++;
			}
			return;
		}
		if (ticks == 0) {
			Screen screen = step == 0 ? new InventoryScreen(minecraft.player) : new PauseScreen(true);
			minecraft.gui.setScreen(screen);
		} else if (ticks == 10 || ticks == 12) {
			Screen s = minecraft.gui.screen();
			if (s == null) {
				return;
			}
			double gx, gy;
			if (step == 0) {
				// The first hotbar slot of the survival inventory (176 x 166, centred; slot at 8,142).
				gx = (s.width - 176) / 2 + 8 + 8;
				gy = (s.height - 166) / 2 + 142 + 8;
			} else {
				gx = s.width / 2.0;
				gy = s.height / 3.0;
				for (GuiEventListener child : s.children()) {
					if (child instanceof AbstractWidget w && w.visible) {
						gx = w.getX() + w.getWidth() * 0.3;
						gy = w.getY() + w.getHeight() * 0.6;
						break;
					}
				}
			}
			var window = minecraft.getWindow();
			double scale = (double) window.getScreenWidth() / window.getGuiScaledWidth();
			double wx = gx * scale, wy = gy * scale;
			minecraft.mouseHandler.onMove(window.handle(), wx, wy, 0.0, 0.0); // as InputBridge does for kInCursor
			if (ticks == 12) {
				WitcherCraft.LOG.info("WitcherCraft: cursor check {}: mouse at window ({}, {}) = gui ({}, {})", step == 0 ? "inventory" : "pause",
					(int) wx, (int) wy, (int) gx, (int) gy);
			}
		} else if (ticks == 30) {
			String name = step == 0 ? "wc_cursor_inventory.png" : "wc_cursor_pause.png";
			Screenshot.grab(minecraft.gameDirectory, name, minecraft.gameRenderer.mainRenderTarget(), 1,
				msg -> WitcherCraft.LOG.info("WitcherCraft: cursor check {}: {}", name, msg.getString()));
			step++;
			ticks = -10;
		}
	}
}
