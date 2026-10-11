package dev.witchercraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.witchercraft.link.SkyLink;
import dev.witchercraft.world.SkyCollision;
import dev.witchercraft.world.SkyTri;
import dev.witchercraft.world.TriCollider;
import java.util.ArrayList;
import java.util.List;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.phys.AABB;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * Crouching stops at edges of the Witcher's ground too. Minecraft looks for block collision under
 * the player to decide where an edge is; the Witcher ground the player walks on isn't blocks (the
 * player collides with its exact triangles), so on its own every direction looked like a drop.
 * Here a spot only counts as a drop if there is neither a block nor Witcher ground under it.
 */
@Mixin(Player.class)
public abstract class PlayerEdgeMixin {
	@WrapOperation(
		method = { "maybeBackOffFromEdge", "isAboveGround" },
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/entity/player/Player;canFallAtLeast(DDD)Z")
	)
	private boolean witchercraft$witcherGroundIsGround(Player player, double deltaX, double deltaZ, double minHeight, Operation<Boolean> original) {
		if (!original.call(player, deltaX, deltaZ, minHeight)) {
			return false; // a Minecraft block is under it
		}
		if (!SkyLink.active()) {
			return true;
		}
		AABB box = player.getBoundingBox().move(deltaX, 0.0, deltaZ);
		List<SkyTri> tris = new ArrayList<>();
		SkyCollision.trianglesNear(new AABB(box.minX - 0.5, box.minY - minHeight - 1.0, box.minZ - 0.5, box.maxX + 0.5, box.minY + 0.5, box.maxZ + 0.5), tris);
		if (tris.isEmpty()) {
			return true;
		}
		// Witcher ground anywhere under the footprint, no more than minHeight below the feet.
		for (int i = 0; i <= 2; i++) {
			for (int k = 0; k <= 2; k++) {
				double x = box.minX + 1.0E-3 + (box.getXsize() - 2.0E-3) * i * 0.5;
				double z = box.minZ + 1.0E-3 + (box.getZsize() - 2.0E-3) * k * 0.5;
				double ground = TriCollider.groundAt(tris, x, box.minY, z, 0.05);
				if (!Double.isNaN(ground) && ground >= box.minY - minHeight) {
					return false;
				}
			}
		}
		return true;
	}
}
