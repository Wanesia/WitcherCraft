package dev.witchercraft.client.mixin;

import dev.witchercraft.client.LedgeVault;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.LivingEntity;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** WitcherCraft M7 climbing help: a jump at a reachable ledge lands on it (LedgeVault). */
@Mixin(LivingEntity.class)
public abstract class JumpVaultMixin {
	@Inject(method = "jumpFromGround", at = @At("TAIL"), require = 0)
	private void witchercraft$vault(CallbackInfo ci) {
		if ((Object) this instanceof LocalPlayer player) {
			LedgeVault.onJump(player);
		}
	}
}
