package dev.witchercraft.client;

import dev.witchercraft.WitcherCraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;

/**
 * WitcherCraft M7 climbing help: The Witcher is full of ledges, crates and walls Geralt climbs but
 * Steve's 1.25 block jump can't. Jumping while walking at one, with a ledge 1.1 to 3.3 blocks up
 * just ahead (the Witcher's geometry or Minecraft blocks) and room to stand on it, the jump gets
 * exactly the strength to land on top, plus a small push forward.
 */
public final class LedgeVault {
	private static final double MIN = 1.1, MAX = 3.3, BODY = 1.8;
	private static int logs;
	// The vault in progress: once the feet clear the ledge, carry Steve over its edge.
	private static double vaultTop, vaultDirX, vaultDirZ;
	private static int vaultUntil = Integer.MIN_VALUE;

	private LedgeVault() {
	}

	/** Every movement of the local player (before collision): the vault's push over the edge. */
	public static Vec3 assist(LocalPlayer p, Vec3 move) {
		if (p.tickCount > vaultUntil) {
			return move;
		}
		if (p.onGround() && p.getY() >= vaultTop - 0.05) {
			vaultUntil = Integer.MIN_VALUE; // standing on it
			return move;
		}
		if (p.getY() < vaultTop - 0.05 || p.zza <= 0.1F) {
			return move;
		}
		double along = move.x * vaultDirX + move.z * vaultDirZ;
		double want = 0.22;
		if (along >= want) {
			return move;
		}
		return new Vec3(move.x + vaultDirX * (want - along), move.y, move.z + vaultDirZ * (want - along));
	}

	/** Right after vanilla's jump set the jump velocity. */
	public static void onJump(LocalPlayer p) {
		if (!dev.witchercraft.WcOptions.climbHelp || !SkyClient.linked() || p.isInWater() || p.isFallFlying() || p.getAbilities().flying || p.zza <= 0.1F) {
			return;
		}
		double yaw = Math.toRadians(p.getYRot());
		double fx = -Math.sin(yaw), fz = Math.cos(yaw);
		// Nearest ledge first: a jump a little before the wall counts too.
		for (double ahead = 0.6; ahead <= 1.31; ahead += 0.35) {
			if (tryVault(p, fx, fz, ahead)) {
				return;
			}
		}
	}

	private static boolean tryVault(LocalPlayer p, double fx, double fz, double ahead) {
		double x = p.getX() + fx * ahead, y = p.getY(), z = p.getZ() + fz * ahead;
		AABB box = p.getBoundingBox();
		double ledge = Double.NaN;

		// The Witcher's geometry: the highest walkable surface ahead within reach, with nothing
		// walkable again within the body's height above it.
		double g = SkyCollider.groundAt(x, y, z, MAX);
		if (!Double.isNaN(g) && g - y > MIN && g - y <= MAX) {
			double above = SkyCollider.groundAt(x, g + BODY, z, 0.0);
			if (Double.isNaN(above) || above <= g + 0.2) {
				ledge = g;
			}
		}
		// Minecraft blocks: the lowest height where the body fits ahead with something under it.
		for (double h = MIN; h <= MAX + 1e-6; h += 0.125) {
			AABB at = box.move(fx * ahead, h, fz * ahead);
			if (p.level().noCollision(p, at) && !p.level().noCollision(p, at.move(0.0, -0.15, 0.0))) {
				double top = y + h;
				if (Double.isNaN(ledge) || top < ledge) {
					ledge = top;
				}
				break;
			}
		}
		if (Double.isNaN(ledge)) {
			return false;
		}
		double rise = ledge - y;
		// Room overhead where Steve is now (Minecraft blocks; a Witcher ceiling stops him as usual).
		if (!p.level().noCollision(p, box.expandTowards(0.0, rise + 0.2, 0.0))) {
			return false;
		}
		// Vanilla's jump (0.42) reaches 1.25 blocks; the apex grows ~ v^2 / 0.141. Half a block of
		// clearance, then assist() carries Steve over the edge.
		double vy = Math.min(0.85, Math.sqrt(0.141 * (rise + 0.6)));
		Vec3 v = p.getDeltaMovement();
		p.setDeltaMovement(v.x, Math.max(v.y, vy), v.z);
		vaultTop = ledge;
		vaultDirX = fx;
		vaultDirZ = fz;
		vaultUntil = p.tickCount + 25;
		if (logs < 50) {
			logs++;
			WitcherCraft.LOG.info("WitcherCraft: ledge vault up {} blocks (jump {}, ledge {} ahead)", String.format("%.2f", rise), String.format("%.2f", vy),
				String.format("%.2f", ahead));
		}
		return true;
	}
}
