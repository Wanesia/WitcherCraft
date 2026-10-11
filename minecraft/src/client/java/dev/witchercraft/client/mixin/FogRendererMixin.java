package dev.witchercraft.client.mixin;

import dev.witchercraft.client.WorldLayer;
import net.minecraft.client.renderer.fog.FogRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.ModifyVariable;

/**
 * WitcherCraft world layer: no Minecraft fog (render-distance, atmospheric, water, ...). The host
 * game has its own fog; Minecraft's would tint distant blocks towards a sky colour that isn't there.
 */
@Mixin(FogRenderer.class)
public abstract class FogRendererMixin {
	@ModifyVariable(method = "getBuffer", at = @At("HEAD"), argsOnly = true)
	private FogRenderer.FogMode witchercraft$noFog(FogRenderer.FogMode mode) {
		return WorldLayer.active() ? FogRenderer.FogMode.NONE : mode;
	}
}
