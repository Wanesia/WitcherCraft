package dev.witchercraft.mixin;

import dev.witchercraft.world.SkyCollision;
import net.minecraft.core.BlockPos;
import net.minecraft.world.entity.ai.navigation.PathNavigation;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** WitcherCraft: the Witcher's ground is somewhere to walk to (wandering, following the player), not a drop. */
@Mixin(PathNavigation.class)
public abstract class MobPathStableMixin {
	@Inject(method = "isStableDestination", at = @At("RETURN"), cancellable = true)
	private void witchercraft$witcherGround(BlockPos pos, CallbackInfoReturnable<Boolean> cir) {
		if (!cir.getReturnValueZ() && SkyCollision.supportsFromBelow(pos)) {
			cir.setReturnValue(true);
		}
	}
}
