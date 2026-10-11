package dev.witchercraft.client.mixin;

import dev.witchercraft.client.WcCursor;
import dev.witchercraft.client.WcPanelPause;
import net.minecraft.client.DeltaTracker;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.Gui;
import net.minecraft.client.renderer.state.gui.GuiRenderState;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * WitcherCraft: Minecraft pauses under an open Witcher panel (WcPanelPause), and
 * draws its own mouse cursor on its screens while linked (WcCursor).
 */
@Mixin(Gui.class)
public abstract class WcGuiMixin {
	@Shadow @Final private Minecraft minecraft;
	@Shadow @Final private GuiRenderState guiRenderState;

	@Inject(method = "isPausing", at = @At("HEAD"), cancellable = true)
	private void witchercraft$pauseUnderWitcherPanel(CallbackInfoReturnable<Boolean> cir) {
		if (WcPanelPause.active()) {
			cir.setReturnValue(true);
		}
	}

	@Inject(method = "extractRenderState", at = @At("TAIL"))
	private void witchercraft$cursor(DeltaTracker deltaTracker, boolean shouldRenderLevel, boolean resourcesLoaded, CallbackInfo ci) {
		WcCursor.extract(this.minecraft, this.guiRenderState, resourcesLoaded);
	}
}
