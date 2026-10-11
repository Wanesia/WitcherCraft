package dev.witchercraft.client.mixin;

import dev.witchercraft.client.SkyClient;
import dev.witchercraft.client.WorldLayer;
import net.minecraft.client.renderer.LevelRenderer;
import org.joml.Vector4f;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.ModifyVariable;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Skyrim draws the world. While linked, Minecraft renders nothing of its own level (no sky,
 * clouds, fog or terrain) so the overlay is just hand + HUD on a transparent background.
 *
 * WitcherCraft world layer: the level IS rendered (blocks, entities, particles, outline), but
 * without the sky and cleared to transparent black (the clear keeps alpha 0 but used the fog
 * colour's RGB, which would add a tint under premultiplied-alpha compositing).
 */
@Mixin(LevelRenderer.class)
public abstract class LevelRendererMixin {
	private static final String RENDER =
		"render(Lcom/mojang/blaze3d/resource/GraphicsResourceAllocator;ZLnet/minecraft/client/renderer/state/level/CameraRenderState;Lcom/mojang/renderpearl/api/buffers/GpuBufferSlice;Lorg/joml/Vector4f;ZZ)V";

	@Inject(method = RENDER, at = @At("HEAD"), cancellable = true)
	private void witchercraft$skipLevel(CallbackInfo ci) {
		if (SkyClient.linked() && !WorldLayer.ENABLED) {
			ci.cancel();
		}
	}

	/** shouldRenderSky (the 2nd boolean argument): no sky box, sun, moon or stars. */
	@ModifyVariable(method = RENDER, at = @At("HEAD"), argsOnly = true, ordinal = 1)
	private boolean witchercraft$noSky(boolean shouldRenderSky) {
		return WorldLayer.active() ? false : shouldRenderSky;
	}

	/** fogColor: the level clear colour is (fog.rgb, 0); make it transparent black. */
	@ModifyVariable(method = RENDER, at = @At("HEAD"), argsOnly = true)
	private Vector4f witchercraft$transparentClear(Vector4f fogColor) {
		return WorldLayer.active() ? new Vector4f(0.0F, 0.0F, 0.0F, 0.0F) : fogColor;
	}
}
