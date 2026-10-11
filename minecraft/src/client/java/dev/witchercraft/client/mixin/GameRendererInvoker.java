package dev.witchercraft.client.mixin;

import com.mojang.blaze3d.vertex.PoseStack;
import net.minecraft.client.renderer.GameRenderer;
import net.minecraft.client.renderer.state.level.CameraRenderState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Invoker;

/**
 * WitcherCraft world layer: Minecraft's own hurt-tilt and walk-bob transforms, so the camera
 * published with each world-layer frame is exactly the one the level was rendered with.
 */
@Mixin(GameRenderer.class)
public interface GameRendererInvoker {
	@Invoker("bobHurt")
	void witchercraft$bobHurt(CameraRenderState cameraState, PoseStack poseStack);

	@Invoker("bobView")
	void witchercraft$bobView(CameraRenderState cameraState, PoseStack poseStack);
}
