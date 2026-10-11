package dev.witchercraft.combat;

import dev.witchercraft.world.SkyClip;
import java.util.EnumSet;
import java.util.function.Predicate;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.util.Mth;
import net.minecraft.world.entity.EntitySelector;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.NeutralMob;
import net.minecraft.world.entity.TamableAnimal;
import net.minecraft.world.entity.ai.goal.Goal;
import net.minecraft.world.entity.ai.goal.GoalSelector;
import net.minecraft.world.entity.ai.goal.target.TargetGoal;
import net.minecraft.world.entity.animal.golem.IronGolem;
import net.minecraft.world.entity.animal.golem.SnowGolem;
import net.minecraft.world.entity.animal.wolf.Wolf;
import net.minecraft.world.entity.monster.Enemy;
import net.minecraft.world.entity.monster.cubemob.Slime;
import net.minecraft.world.entity.monster.spider.Spider;
import net.minecraft.world.entity.monster.zombie.Drowned;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.phys.Vec3;
import org.jspecify.annotations.Nullable;

/**
 * WitcherCraft: Minecraft mobs fight the Witcher's enemies. A mob with this goal targets the nearest
 * hostile Witcher actor's stand-in (never a friendly or neutral NPC, see
 * {@link SkyrimActorEntity#isW3Enemy}) that it can see within its follow range, clamped to
 * {@value #MIN_RANGE}..{@value #MAX_RANGE} blocks. The mob's own attack goals then treat the stand-in
 * like any target (a zombie's swing, a skeleton's bow, a creeper's fuse, an iron golem's slam), and
 * the hits reach the Witcher as hits by a mob (Proto.HIT_BY_MOB: Geralt gets no aggro or kill credit).
 *
 * <p>Hostile mobs keep attacking players too, and take whichever is closer: the goal outranks their
 * player goals, so it switches a mob from a player to a closer enemy, and it lets go of the enemy when
 * a player they would attack comes clearly closer. Iron golems, snow golems and tamed wolves (not
 * sitting) only add the Witcher's enemies to what they already fight; their village and owner
 * defence comes first.
 *
 * <p>Adapted from Minecraft-Ring's ErEnemyTargetGoal (MIT, see THIRD-PARTY-NOTICES.md).
 */
public final class WitcherEnemyTargetGoal extends TargetGoal {
	private static final double MIN_RANGE = 16.0;
	private static final double MAX_RANGE = 24.0;
	/** A player has to be this much closer (blocks) than the enemy to win a hostile mob back. */
	private static final double PLAYER_MARGIN = 2.0;

	private final boolean alsoFightsPlayers;
	private final Predicate<Mob> willing;
	private @Nullable SkyrimActorEntity candidate;
	private int unseenTicks;

	private WitcherEnemyTargetGoal(Mob mob, boolean alsoFightsPlayers, Predicate<Mob> willing) {
		super(mob, true);
		this.alsoFightsPlayers = alsoFightsPlayers;
		this.willing = willing;
		this.setFlags(EnumSet.of(Goal.Flag.TARGET));
	}

	/**
	 * Adds the goal to the mobs that fight the Witcher's enemies (MobAlliesMixin: every new
	 * server-side mob, after its own goals). Lower priority numbers win.
	 */
	public static void addTo(Mob mob, GoalSelector targets) {
		if (mob instanceof Enemy) {
			// Hostile mobs whose targeting is goal-driven (brain-driven ones, like piglins, hoglins,
			// wardens and breezes, have no target goals). Slimes only hurt players and iron golems,
			// by touching them.
			if (!(mob instanceof Slime) && !targets.getAvailableGoals().isEmpty()) {
				targets.addGoal(0, new WitcherEnemyTargetGoal(mob, true, m -> true));
			}
		} else if (mob instanceof IronGolem) {
			targets.addGoal(2, new WitcherEnemyTargetGoal(mob, false, m -> true)); // after defending its village
		} else if (mob instanceof SnowGolem) {
			targets.addGoal(1, new WitcherEnemyTargetGoal(mob, false, m -> true));
		} else if (mob instanceof Wolf) {
			// Tamed and not told to sit; after defending its owner.
			targets.addGoal(2, new WitcherEnemyTargetGoal(mob, false, m -> m instanceof TamableAnimal t && t.isTame() && !t.isOrderedToSit()));
		}
	}

	@Override
	protected double getFollowDistance() {
		return Mth.clamp(super.getFollowDistance(), MIN_RANGE, MAX_RANGE);
	}

	@Override
	public boolean canUse() {
		if (!SkyCombat.hasEnemies() || this.mob.getRandom().nextInt(reducedTickDelay(5)) != 0 || !this.willing.test(this.mob)) {
			return false;
		}
		SkyrimActorEntity enemy = nearestVisibleEnemy(this.mob, this.getFollowDistance());
		if (enemy == null) {
			return false;
		}
		double dist = Math.sqrt(hitboxDistanceSqr(enemy, this.mob.position()));
		LivingEntity current = this.mob.getTarget();
		if (current != null && current.isAlive() && !(current instanceof SkyrimActorEntity) && this.mob.distanceTo(current) < dist) {
			return false; // already after something closer
		}
		if (this.alsoFightsPlayers && this.playerClearlyCloser(dist)) {
			return false; // leave the player to the mob's own goals
		}
		this.candidate = enemy;
		return true;
	}

	/**
	 * TargetGoal's own test, but with the range measured to the hitbox like {@link #canUse}
	 * (Minecraft measures to the stand-in's feet, which would drop a big monster picked near the
	 * range's edge right away).
	 */
	@Override
	public boolean canContinueToUse() {
		if (!(this.mob.getTarget() instanceof SkyrimActorEntity enemy) || !this.mob.canAttack(enemy) || !this.willing.test(this.mob)) {
			return false; // canAttack: gone, dead, or no longer hostile
		}
		double range = this.getFollowDistance();
		double distSq = hitboxDistanceSqr(enemy, this.mob.position());
		if (distSq > range * range) {
			return false;
		}
		if (this.mob.getSensing().hasLineOfSight(enemy)) {
			this.unseenTicks = 0;
		} else if (++this.unseenTicks > reducedTickDelay(this.unseenMemoryTicks)) {
			return false;
		}
		return !(this.alsoFightsPlayers && this.playerClearlyCloser(Math.sqrt(distSq)));
	}

	@Override
	public void start() {
		this.mob.setTarget(this.candidate);
		this.unseenTicks = 0;
		super.start();
	}

	/** Clears the mob's target only if it is still the enemy (another goal or an alert may have given it a new one). */
	@Override
	public void stop() {
		if (this.mob.getTarget() instanceof SkyrimActorEntity) {
			super.stop();
		}
		this.candidate = null;
	}

	/** A player this mob would attack is in sight and at least {@link #PLAYER_MARGIN} closer than an enemy {@code enemyDist} away. */
	private boolean playerClearlyCloser(double enemyDist) {
		double within = enemyDist - PLAYER_MARGIN;
		if (within <= 0.0) {
			return false;
		}
		Player player = this.mob.level().getNearestPlayer(this.mob.getX(), this.mob.getY(), this.mob.getZ(), within, EntitySelector.NO_CREATIVE_OR_SPECTATOR);
		return player != null && this.wouldFight(player) && this.mob.getSensing().hasLineOfSight(player);
	}

	/**
	 * Whether the mob's own goals go after {@code player} at all right now: neutral mobs (endermen,
	 * zombified piglins) only when angry at them, spiders only in the dark, drowned by their own rule.
	 * Otherwise it keeps fighting the enemy rather than stand idle next to the player.
	 */
	@SuppressWarnings("deprecation") // the light test Spider's own target goal uses
	private boolean wouldFight(Player player) {
		if (!this.mob.canAttack(player)) {
			return false;
		}
		if (this.mob instanceof NeutralMob neutral && this.mob.level() instanceof ServerLevel level && !neutral.isAngryAt(player, level)) {
			return false;
		}
		if (this.mob instanceof Spider && this.mob.getLightLevelDependentMagicValue() >= 0.5F) {
			return false;
		}
		return !(this.mob instanceof Drowned drowned) || drowned.okTarget(player);
	}

	/**
	 * The nearest enemy stand-in {@code mob} can see, its hitbox within {@code range} blocks of the
	 * mob's feet, or null. Seeing takes both Minecraft's blocks and the Witcher's walls into account.
	 */
	private static @Nullable SkyrimActorEntity nearestVisibleEnemy(Mob mob, double range) {
		Vec3 feet = mob.position();
		SkyrimActorEntity best = null;
		double bestSq = range * range;
		for (SkyrimActorEntity e : SkyCombat.proxies()) {
			if (e.level() != mob.level() || !e.isW3Enemy() || !e.isAlive()) {
				continue;
			}
			double d = hitboxDistanceSqr(e, feet);
			if (d < bestSq && mob.getSensing().hasLineOfSight(e)
				&& SkyClip.cast(mob.getEyePosition(), new Vec3(e.getX(), e.getEyeY(), e.getZ())) == null) {
				best = e;
				bestSq = d;
			}
		}
		return best;
	}

	private static double hitboxDistanceSqr(SkyrimActorEntity e, Vec3 pos) {
		return e.getBoundingBox().distanceToSqr(pos);
	}
}
