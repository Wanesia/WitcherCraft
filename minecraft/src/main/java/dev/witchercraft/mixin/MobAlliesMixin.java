package dev.witchercraft.mixin;

import dev.witchercraft.combat.WitcherEnemyTargetGoal;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.ai.goal.GoalSelector;
import net.minecraft.world.level.Level;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * WitcherCraft: hostile mobs, golems and tamed wolves fight the Witcher's enemies
 * ({@link WitcherEnemyTargetGoal}). The goal goes in after the mob's own goals, which Mob's
 * constructor registers on the server side.
 */
@Mixin(Mob.class)
public abstract class MobAlliesMixin {
	@Shadow
	@Final
	protected GoalSelector targetSelector;

	@Inject(method = "<init>", at = @At("TAIL"))
	private void witchercraft$fightWitcherEnemies(EntityType<? extends Mob> type, Level level, CallbackInfo ci) {
		if (level != null && !level.isClientSide()) {
			WitcherEnemyTargetGoal.addTo((Mob) (Object) this, this.targetSelector);
		}
	}
}
