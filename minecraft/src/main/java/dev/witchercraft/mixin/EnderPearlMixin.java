package dev.witchercraft.mixin;

import dev.witchercraft.world.SkyCollision;
import dev.witchercraft.world.SkyPearl;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.projectile.throwableitemprojectile.ThrownEnderpearl;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * WitcherCraft: an ender pearl about to fly into a column Minecraft has no Witcher collision for
 * waits in the air (no move, no gravity) until the host has sent it. Without this, pearls thrown
 * past the collision around Steve fell through the world. If the collision never comes within
 * SkyPearl.MAX_HOLD_TICKS (The Witcher hasn't loaded that far yet), the pearl lands where it waits:
 * Steve is teleported there and held by the usual "no ground yet" hold until the ground streams in.
 */
@Mixin(ThrownEnderpearl.class)
public abstract class EnderPearlMixin {
	@Unique
	private int witchercraft$heldTicks;
	@Unique
	private boolean witchercraft$waiting;

	@Shadow
	protected abstract void onHit(HitResult hitResult);

	@Inject(method = "tick", at = @At("HEAD"), cancellable = true)
	private void witchercraft$waitForCollision(CallbackInfo ci) {
		ThrownEnderpearl self = (ThrownEnderpearl) (Object) this;
		if (!(self.level() instanceof ServerLevel) || !SkyCollision.active() || !(self.getOwner() instanceof ServerPlayer)) {
			return;
		}
		Vec3 v = self.getDeltaMovement();
		Vec3 next = self.position().add(v);
		boolean unknown = !SkyPearl.known(next.x, next.y, next.z);
		if (unknown && witchercraft$heldTicks >= SkyPearl.MAX_HOLD_TICKS) {
			dev.witchercraft.WitcherCraft.LOG.info("WitcherCraft: pearl waited {} ticks at {} for collision; landing it there", witchercraft$heldTicks, self.position());
			SkyPearl.report(self.position(), Vec3.ZERO, false);
			this.onHit(new BlockHitResult(self.position(), Direction.UP, BlockPos.containing(self.position()), false));
			ci.cancel();
			return;
		}
		SkyPearl.report(self.position(), v, unknown);
		if (!unknown && witchercraft$waiting) {
			// Perf: how long the pearl hung in the air (ticks in total for this pearl).
			witchercraft$waiting = false;
			dev.witchercraft.WitcherCraft.LOG.info("WitcherCraft: pearl flies on after waiting for collision ({} ticks so far)", witchercraft$heldTicks);
		}
		witchercraft$waiting = unknown;
		if (unknown) {
			if (witchercraft$heldTicks == 0) {
				dev.witchercraft.WitcherCraft.LOG.info("WitcherCraft: pearl held at {} until the Witcher's collision there is in", self.position());
			}
			witchercraft$heldTicks++;
			ci.cancel();
		}
	}
}
