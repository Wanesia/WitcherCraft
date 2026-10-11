package dev.witchercraft.client;

import dev.witchercraft.WitcherCraft;
import dev.witchercraft.world.SkyCollision;
import dev.witchercraft.world.SkyPearl;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.phys.Vec3;

/**
 * WitcherCraft: smooth flight at the edge of the known world. Fast (elytra, rockets) Steve can
 * outrun the Witcher collision the host streams in; the host's flight guard then froze him (a
 * teleport hold that also killed the elytra's speed: "laggy flying, stuck on nothing"). Here, each
 * tick, the move is shortened so it stops short of a column Minecraft has no collision for yet,
 * while his velocity is kept: as soon as the column arrives he flies on at full speed.
 */
public final class FlightBrake {
	private static final double MIN_MOVE = 0.45; // blocks per tick (9 blocks/s): walking is never braked
	private static final double TICKS_AHEAD = 3.0, MARGIN = 1.5;
	private static int brakes;
	private static long lastLogMs;

	private FlightBrake() {
	}

	/** The local player's own move this tick (before collision). */
	public static Vec3 limit(LocalPlayer p, Vec3 move) {
		if (!SkyClient.linked() || !SkyCollision.active() || p.noPhysics) {
			return move;
		}
		double len = move.length();
		if (len < MIN_MOVE) {
			return move;
		}
		// Where Steve is has no Witcher surface at all (not loaded there either): nothing to compare with.
		if (!SkyPearl.known(p.getX(), p.getY(), p.getZ())) {
			return move;
		}
		Vec3 dir = move.scale(1.0 / len);
		double reach = len * TICKS_AHEAD + MARGIN;
		double cy = p.getY() + p.getBbHeight() * 0.5;
		for (double d = 1.0; d <= reach; d += 1.0) {
			double x = p.getX() + dir.x * d, y = cy + dir.y * d, z = p.getZ() + dir.z * d;
			if (!SkyPearl.known(x, y, z)) {
				double allowed = Math.max(0.0, d - MARGIN);
				if (allowed >= len) {
					return move;
				}
				brakes++;
				long now = System.currentTimeMillis();
				if (now - lastLogMs > 2000) {
					lastLogMs = now;
					WitcherCraft.LOG.info("WitcherCraft: flight brake at {} blocks/s ({} so far): collision ahead not in yet", String.format("%.1f", len * 20.0),
						brakes);
				}
				return dir.scale(allowed);
			}
		}
		return move;
	}
}
