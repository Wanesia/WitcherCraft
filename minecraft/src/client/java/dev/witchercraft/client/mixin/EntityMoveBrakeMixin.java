package dev.witchercraft.client.mixin;

import dev.witchercraft.client.FlightBrake;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.ModifyVariable;

/**
 * WitcherCraft: the local player's own move is shortened at the edge of the known Witcher world
 * (FlightBrake). Done on move()'s argument, not collide()'s, so vanilla doesn't take the shorter
 * move for a wall hit (no velocity reset, no elytra crash damage).
 */
@Mixin(Entity.class)
public abstract class EntityMoveBrakeMixin {
	@ModifyVariable(method = "move", at = @At("HEAD"), argsOnly = true)
	private Vec3 witchercraft$flightBrake(Vec3 movement) {
		if ((Object) this instanceof LocalPlayer player) {
			return FlightBrake.limit(player, movement);
		}
		return movement;
	}
}
