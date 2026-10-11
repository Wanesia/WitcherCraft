package dev.witchercraft.client.mixin;

import dev.witchercraft.client.WorldLayer;
import net.minecraft.client.CloudStatus;
import net.minecraft.client.Options;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** WitcherCraft world layer: no Minecraft clouds (the player's saved option is left alone). */
@Mixin(Options.class)
public abstract class OptionsCloudsMixin {
	@Inject(method = "getCloudStatus", at = @At("HEAD"), cancellable = true)
	private void witchercraft$noClouds(CallbackInfoReturnable<CloudStatus> cir) {
		if (WorldLayer.active()) {
			cir.setReturnValue(CloudStatus.OFF);
		}
	}
}
