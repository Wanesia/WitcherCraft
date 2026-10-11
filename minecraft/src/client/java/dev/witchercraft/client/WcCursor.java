package dev.witchercraft.client;

import com.mojang.blaze3d.platform.Window;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.renderer.state.gui.GuiRenderState;
import org.joml.Matrix3x2fStack;

/**
 * WitcherCraft: the mouse cursor on Minecraft's screens (inventory, menus, chat) while linked.
 *
 * Minecraft's window is hidden; its frame is shown inside the Witcher, which has no cursor of its own
 * there (and the host hides the Windows cursor while Minecraft has the mouse). So Minecraft draws the
 * cursor itself, on top of everything in its GUI pass (WcGuiMixin, end of Gui.extractRenderState): it
 * travels inside the overlay frame, in the same frame as the screen, at exactly the position Minecraft
 * uses for hover and clicks (MouseHandler, fed by the host's kInCursor events).
 */
public final class WcCursor {
	// A classic arrow in "cursor pixels" (B outline, W fill, space = clear); the tip is (0, 0).
	private static final String[] ARROW = {
		"B",
		"BB",
		"BWB",
		"BWWB",
		"BWWWB",
		"BWWWWB",
		"BWWWWWB",
		"BWWWWWWB",
		"BWWWWWWWB",
		"BWWWWWWWWB",
		"BWWWWWWWWWB",
		"BWWWWWWBBBBB",
		"BWWWBWWB",
		"BWWB BWWB",
		"BWB  BWWB",
		"BB    BWWB",
		"B     BWWB",
		"       BWWB",
		"        BB",
	};
	private static final int OUTLINE = 0xFF000000;
	private static final int FILL = 0xFFFFFFFF;

	static {
		WcCursorCheck.register();
	}

	private WcCursor() {
	}

	/** Draw it (or not): called at the end of every GUI extraction. */
	public static void extract(Minecraft minecraft, GuiRenderState state, boolean resourcesLoaded) {
		if (!resourcesLoaded || !SkyClient.linked() || minecraft.gui.screen() == null || minecraft.mouseHandler.isMouseGrabbed()) {
			return;
		}
		Window window = minecraft.getWindow();
		if (window.getScreenWidth() <= 0 || window.getGuiScaledWidth() <= 0) {
			return;
		}
		double gx = minecraft.mouseHandler.getScaledXPos(window);
		double gy = minecraft.mouseHandler.getScaledYPos(window);
		double guiScale = (double) window.getScreenWidth() / window.getGuiScaledWidth();
		// One cursor pixel = 1 screen pixel up to 1350 lines, 2 above (Windows' own cursor scales too).
		int unit = Math.max(1, (int) Math.round(window.getScreenHeight() / 900.0));
		float s = (float) (unit / guiScale);
		GuiGraphicsExtractor g = new GuiGraphicsExtractor(minecraft, state, (int) gx, (int) gy);
		g.nextStratum(); // above the screen, its tooltips and toasts
		Matrix3x2fStack pose = g.pose();
		pose.pushMatrix();
		pose.translate((float) gx, (float) gy);
		pose.scale(s, s);
		for (int y = 0; y < ARROW.length; y++) {
			String row = ARROW[y];
			int x = 0;
			while (x < row.length()) {
				char c = row.charAt(x);
				int end = x + 1;
				while (end < row.length() && row.charAt(end) == c) {
					end++;
				}
				if (c != ' ') {
					g.fill(x, y, end, y + 1, c == 'B' ? OUTLINE : FILL);
				}
				x = end;
			}
		}
		pose.popMatrix();
	}
}
