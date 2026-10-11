package dev.witchercraft.mixin;

import net.minecraft.world.entity.animal.equine.Horse;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Invoker;

/** WitcherCraft: Roach's coat (Horse.setVariantAndMarkings is private). */
@Mixin(Horse.class)
public interface HorseInvoker {
	@Invoker("setVariantAndMarkings")
	void witchercraft$setVariantAndMarkings(net.minecraft.world.entity.animal.equine.Variant variant, net.minecraft.world.entity.animal.equine.Markings markings);
}
