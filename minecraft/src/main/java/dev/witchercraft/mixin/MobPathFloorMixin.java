package dev.witchercraft.mixin;

import dev.witchercraft.world.SkyCollision;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.pathfinder.WalkNodeEvaluator;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * WitcherCraft: the floor height a path node stands on includes the Witcher's ground (see
 * MobPathTypeMixin): the top of low ground inside the node's own cell, or of the ground in the cell
 * below. Step and jump checks then compare real heights.
 */
@Mixin(WalkNodeEvaluator.class)
public abstract class MobPathFloorMixin {
	@Inject(method = "getFloorLevel(Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;)D", at = @At("RETURN"), cancellable = true)
	private static void witchercraft$witcherFloor(BlockGetter level, BlockPos pos, CallbackInfoReturnable<Double> cir) {
		float here = SkyCollision.groundTop(pos.getX(), pos.getY(), pos.getZ());
		double floor;
		if (here > 0.0F && here < 0.5F && level.getBlockState(pos).isAir()) {
			floor = pos.getY() + here;
		} else {
			float below = SkyCollision.groundTop(pos.getX(), pos.getY() - 1, pos.getZ());
			if (below <= 0.0F || !level.getBlockState(pos.below()).isAir()) {
				return;
			}
			floor = pos.getY() - 1 + below;
		}
		if (floor > cir.getReturnValueD()) {
			cir.setReturnValue(floor);
		}
	}
}
