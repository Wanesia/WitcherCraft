package dev.witchercraft.mixin;

import dev.witchercraft.world.SkyCollision;
import net.minecraft.core.BlockPos;
import net.minecraft.world.entity.ai.navigation.GroundPathNavigation;
import net.minecraft.world.level.chunk.LevelChunk;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * WitcherCraft: a walking mob's path goal stays where it was asked for (a Witcher NPC's feet, the
 * player, a spot on the Witcher's ground). Vanilla moves a goal in the air down to the first real
 * block, and under the Witcher's ground there is none: it ended up at the top of the world.
 */
@Mixin(GroundPathNavigation.class)
public abstract class MobPathTargetMixin {
	@Inject(method = "findSurfacePosition", at = @At("HEAD"), cancellable = true)
	private void witchercraft$keepWitcherTarget(LevelChunk chunk, BlockPos pos, int reachRange, CallbackInfoReturnable<BlockPos> cir) {
		if (SkyCollision.active() && chunk.getBlockState(pos).isAir()) {
			cir.setReturnValue(pos);
		}
	}
}
