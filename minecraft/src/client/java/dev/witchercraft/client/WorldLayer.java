package dev.witchercraft.client;

/**
 * WitcherCraft world layer (-Dwitchercraft.worldLayer=true).
 *
 * Upstream SkyCraft never renders Minecraft's level (Skyrim draws the blocks from exported
 * meshes), so its overlay frame is hand + HUD only. With the world layer on, Minecraft renders
 * its level again, but as a layer: no sky, sun, moon, stars, clouds or fog, cleared to
 * transparent black. The overlay frame is then the whole Minecraft picture (blocks, entities,
 * particles, block cracks, selection outline, hand, HUD) on a transparent background, premultiplied,
 * which the host composites over its own frame from the same camera.
 *
 * On by default; -Dwitchercraft.worldLayer=false behaves exactly like upstream SkyCraft (HUD only).
 */
public final class WorldLayer {
	public static final boolean ENABLED = !"false".equals(System.getProperty("witchercraft.worldLayer", "true"));

	private WorldLayer() {
	}

	/** World layer wanted and the host is linked (unlinked, MC behaves like plain Minecraft). */
	public static boolean active() {
		return ENABLED && SkyClient.linked();
	}
}
