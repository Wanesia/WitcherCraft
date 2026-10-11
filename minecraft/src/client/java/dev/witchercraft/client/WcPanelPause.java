package dev.witchercraft.client;

import dev.witchercraft.WitcherCraft;
import dev.witchercraft.link.SkyLink;

/**
 * WitcherCraft: while the Witcher's map, journal, inventory or
 * character panel is open over Minecraft mode, the host sets a WcPace flag and Minecraft pauses
 * (WcGuiMixin: Gui.isPausing), the way singleplayer pauses under its own menu: Steve keeps his exact
 * place and speed (also mid-air, flying or swimming), mobs and time wait. Minecraft's own screens
 * still never pause while linked (ScreenMixin); only the Witcher's panels do, and they pause the
 * Witcher too.
 */
public final class WcPanelPause {
	/** WcPace flags bit set by the host (native control.cpp kPacePanel). */
	public static final int PACE_PANEL = dev.witchercraft.link.Proto.PACE_PANEL;
	private static final SkyLink.Pace PACE = new SkyLink.Pace();
	private static boolean was;

	private WcPanelPause() {
	}

	/** A Witcher panel is open over Minecraft mode right now (the host refreshes it every ~16 ms). */
	public static boolean active() {
		if (!SkyClient.linked()) {
			return false;
		}
		boolean on;
		synchronized (PACE) {
			on = SkyLink.readPace(PACE) && (PACE.flags & PACE_PANEL) != 0 && PACE.fresh();
		}
		if (on != was) {
			was = on;
			WitcherCraft.LOG.info("WitcherCraft: {}", on ? "a Witcher panel is open over Minecraft mode: Minecraft paused"
				: "the Witcher panel is closed: Minecraft runs again");
		}
		return on;
	}
}
