package dev.witchercraft.client.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.witchercraft.client.SkyClient;
import net.minecraft.client.OptionInstance;
import net.minecraft.client.Options;
import net.minecraft.client.gui.components.AbstractWidget;
import net.minecraft.client.gui.components.Tooltip;
import net.minecraft.client.gui.screens.options.OptionsScreen;
import net.minecraft.network.chat.Component;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * The FOV slider is greyed out while linked: the Witcher camera can't follow wide Minecraft FOVs,
 * so blocks would no longer line up with the Witcher world (SkyClient.LOCKED_FOV).
 */
@Mixin(OptionsScreen.class)
public abstract class OptionsScreenFovMixin {
	@WrapOperation(
		method = "init",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/client/OptionInstance;createButton(Lnet/minecraft/client/Options;)Lnet/minecraft/client/gui/components/AbstractWidget;")
	)
	private AbstractWidget witchercraft$lockFovSlider(OptionInstance<?> option, Options options, Operation<AbstractWidget> original) {
		AbstractWidget button = original.call(option, options);
		if (SkyClient.linked() && option == options.fov()) {
			button.active = false;
			button.setTooltip(Tooltip.create(Component.literal(
				"Locked at " + SkyClient.LOCKED_FOV + " while playing in The Witcher 3, so blocks line up with the Witcher world.")));
		}
		return button;
	}
}
