package dev.witchercraft.mixin;

import dev.witchercraft.world.SkyCollision;
import net.minecraft.world.level.CollisionGetter;
import net.minecraft.world.level.pathfinder.PathType;
import net.minecraft.world.level.pathfinder.PathfindingContext;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * WitcherCraft: mobs plan paths over the Witcher's ground. Pathfinding only reads block states, and
 * to Minecraft the Witcher's terrain is air, so a mob saw a bottomless drop all around it and stood
 * still. A cell the Witcher's geometry reaches into is BLOCKED when it fills the cell's upper half
 * (stand on top of it, like a slab or a block) and WALKABLE when it only covers the bottom (stand in
 * it; the cell above then counts as open air over a floor). Answered before the level's path type
 * cache, which holds only vanilla answers, so terrain streamed in later is never hidden by it.
 */
@Mixin(PathfindingContext.class)
public abstract class MobPathTypeMixin {
	@Shadow
	@Final
	private CollisionGetter level;

	@Inject(method = "getPathTypeFromState", at = @At("HEAD"), cancellable = true)
	private void witchercraft$witcherGround(int x, int y, int z, CallbackInfoReturnable<PathType> cir) {
		float top = SkyCollision.groundTop(x, y, z);
		if (top > 0.0F && this.level.getBlockState(new net.minecraft.core.BlockPos(x, y, z)).isAir()) {
			cir.setReturnValue(top >= 0.5F ? PathType.BLOCKED : PathType.WALKABLE);
		}
	}
}
