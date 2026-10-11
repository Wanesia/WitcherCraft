package dev.witchercraft.client.mixin;

import dev.witchercraft.client.DepthExport;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.GameRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** WitcherCraft occlusion: the three points of the frame DepthExport needs. */
@Mixin(GameRenderer.class)
public abstract class GameRendererDepthMixin {
	/** render3dHud clears the depth buffer before the hand: keep the level's depth first. */
	@Inject(method = "render3dHud", at = @At(value = "INVOKE",
		target = "Lcom/mojang/renderpearl/api/commands/CommandEncoder;clearDepthTexture(Lcom/mojang/renderpearl/api/textures/GpuTexture;D)V"),
		require = 0)
	private void witchercraft$saveLevelDepth(CallbackInfo ci) {
		DepthExport.saveLevelDepth(Minecraft.getInstance());
	}

	/** Right after renderLevel() (level + hand), before post effects and the GUI. */
	@Inject(method = "render", at = @At(value = "INVOKE",
		target = "Lnet/minecraft/client/renderer/GameRenderer;tryTakeScreenshotIfNeeded()V"), require = 0)
	private void witchercraft$finishDepth(CallbackInfo ci) {
		DepthExport.finishFrame(Minecraft.getInstance());
	}

	/** Just before the 2D GUI renders: note the screen tiles it covers. */
	@Inject(method = "render", at = @At(value = "INVOKE",
		target = "Lnet/minecraft/client/gui/render/GuiRenderer;render()V"), require = 0)
	private void witchercraft$guiMask(CallbackInfo ci) {
		GameRenderer self = (GameRenderer) (Object) this;
		DepthExport.captureGuiMask(Minecraft.getInstance(), self.gameRenderState().guiRenderState);
	}
}
