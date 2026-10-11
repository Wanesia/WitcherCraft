package dev.witchercraft.client.mixin;

import net.minecraft.client.Options;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Started hidden by The Witcher: no accessibility onboarding screen. Minecraft shows it on its very
 * first start (no options.txt yet) and reads its prompt aloud, which a hidden window turns into a voice
 * from nowhere. Options.load runs in Minecraft's constructor, before the first screens are chosen
 * (Gui.addInitialScreens), and returns early when options.txt doesn't exist yet: hence every RETURN.
 */
@Mixin(Options.class)
public abstract class OptionsHiddenStartMixin {
	@Shadow
	public boolean onboardAccessibility;

	@Inject(method = "load", at = @At("RETURN"))
	private void witchercraft$noOnboardingWhenHidden(CallbackInfo ci) {
		if (Boolean.getBoolean("witchercraft.startHidden")) {
			this.onboardAccessibility = false;
		}
	}
}
