package dev.witchercraft.world;

import net.minecraft.world.phys.Vec3;

/**
 * WitcherCraft: the linked player's ender pearl in flight (server thread writes, render thread
 * reads into McState). The host scans and sends the Witcher's collision along its path; until a
 * region the pearl is about to enter is known, the pearl waits in the air (a pearl flying into
 * space Minecraft has no collision for fell through the world: "pearls sometimes don't work").
 */
public final class SkyPearl {
	/** Longest a pearl waits for collision (then it flies on as vanilla). */
	public static final int MAX_HOLD_TICKS = 60;

	private static volatile double x, y, z, vx, vy, vz;
	private static volatile boolean held;
	private static volatile long updatedMs;

	private SkyPearl() {
	}

	/** Server thread, each tick of the player's pearl. Velocity in blocks per tick. */
	public static void report(Vec3 pos, Vec3 deltaPerTick, boolean isHeld) {
		x = pos.x;
		y = pos.y;
		z = pos.z;
		vx = deltaPerTick.x * 20.0;
		vy = deltaPerTick.y * 20.0;
		vz = deltaPerTick.z * 20.0;
		held = isHeld;
		updatedMs = System.currentTimeMillis();
	}

	/** True while a pearl reported within the last 200 ms; fills {x, y, z, vx, vy, vz}. */
	public static boolean current(double[] out) {
		if (System.currentTimeMillis() - updatedMs > 200) {
			return false;
		}
		out[0] = x;
		out[1] = y;
		out[2] = z;
		out[3] = vx;
		out[4] = vy;
		out[5] = vz;
		return true;
	}

	public static boolean held() {
		return held;
	}

	/**
	 * Is the Witcher's collision in for the column at (x, z) under height y? Some Witcher surface
	 * anywhere below (or up to 16 above) must be there: a column sent before The Witcher had loaded
	 * it is empty (it counted as "known" and pearls flew through it into the void). Ground far below
	 * (a cliff, flying high) counts.
	 */
	public static boolean known(double px, double py, double pz) {
		return SkyCollision.hasGeometry(px, py, pz, 512, 16);
	}
}
