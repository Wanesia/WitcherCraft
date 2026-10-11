package dev.witchercraft.combat;

import dev.witchercraft.link.Proto;
import net.minecraft.core.BlockPos;
import net.minecraft.network.syncher.EntityDataAccessor;
import net.minecraft.network.syncher.EntityDataSerializers;
import net.minecraft.network.syncher.SynchedEntityData;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.sounds.SoundEvent;
import net.minecraft.sounds.SoundEvents;
import net.minecraft.sounds.SoundSource;
import net.minecraft.tags.FluidTags;
import net.minecraft.world.InteractionHand;
import net.minecraft.world.InteractionResult;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.level.block.BaseFireBlock;
import net.minecraft.world.level.block.CampfireBlock;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.material.FluidState;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EntityDimensions;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.HumanoidArm;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.Pose;
import net.minecraft.tags.ItemTags;
import net.minecraft.world.entity.projectile.Projectile;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.level.Level;
import org.jspecify.annotations.Nullable;

/**
 * An invisible stand-in for one Skyrim actor, so Minecraft's own combat (swords, crits, sweeps,
 * enchantments, attack cooldown, bows, tridents) can target and hit Skyrim NPCs. What it receives is
 * collected into one hit per tick and forwarded to the real actor; its own health never drops.
 */
public class SkyrimActorEntity extends LivingEntity {
	private static final EntityDataAccessor<Integer> FORM_ID = SynchedEntityData.defineId(SkyrimActorEntity.class, EntityDataSerializers.INT);
	private static final EntityDataAccessor<Float> WIDTH = SynchedEntityData.defineId(SkyrimActorEntity.class, EntityDataSerializers.FLOAT);
	private static final EntityDataAccessor<Float> HEIGHT = SynchedEntityData.defineId(SkyrimActorEntity.class, EntityDataSerializers.FLOAT);

	// This tick's hit, flushed to Skyrim by SkyCombat after all attacks for the tick have landed
	// (Player.attack adds its sprint/enchantment knockback after hurtServer returns).
	private float pendingDamage;
	private int pendingFlags;
	private int pendingWeapon;
	private double pushX, pushZ;
	private float pushStrength;
	private boolean hitThisTick;
	// WitcherCraft: who dealt this tick's damage. Only mobs (their swings, arrows, explosions) ->
	// Proto.HIT_BY_MOB; the player's hits and damage from no one (lava, fire) stay the player's.
	private boolean hitByPlayer, hitByMob;
	// WitcherCraft: hostile to Geralt and able to take damage, so Minecraft mobs may fight it (SkyCombat.isEnemy).
	private boolean w3Enemy;

	// WitcherCraft fire: the actor burns in the Witcher (actor table), water touched us this tick,
	// and when we last told the Witcher to set it burning (game time).
	private boolean w3Burning;
	private boolean w3NoFire; // the Witcher can't set it burning (actor table ACTOR_NO_FIRE)
	private boolean doused;
	private long igniteSentAt = Long.MIN_VALUE / 2;

	public SkyrimActorEntity(EntityType<? extends SkyrimActorEntity> type, Level level) {
		super(type, level);
		this.setNoGravity(true);
		this.noPhysics = true;
		this.setInvisible(true);
		this.setSilent(true);
	}

	// WitcherCraft hit boxes: where the actor table last put us, and the actor's smoothed velocity
	// (blocks/s), so the stand-in is placed where the Witcher draws the actor NOW, not ~0.1 s ago.
	private static final double LEAD_S = 0.1;        // table age + Minecraft's tick
	private static final double MAX_EXTRAPOLATE_S = 0.25;
	private static final double MAX_SPEED = 12.0;
	private double tableX, tableY, tableZ, velX, velZ;
	private long tableTick = Long.MIN_VALUE;

	public int formId() {
		return this.entityData.get(FORM_ID);
	}

	public void setFormId(int formId) {
		this.entityData.set(FORM_ID, formId);
	}

	@Override
	protected void defineSynchedData(SynchedEntityData.Builder builder) {
		super.defineSynchedData(builder);
		builder.define(FORM_ID, 0);
		builder.define(WIDTH, 0.6F);
		builder.define(HEIGHT, 1.8F);
	}

	public void setSize(float width, float height) {
		if (Math.abs(this.entityData.get(WIDTH) - width) > 0.01F || Math.abs(this.entityData.get(HEIGHT) - height) > 0.01F) {
			this.entityData.set(WIDTH, width);
			this.entityData.set(HEIGHT, height);
			this.refreshDimensions();
		}
	}

	@Override
	public void onSyncedDataUpdated(EntityDataAccessor<?> accessor) {
		super.onSyncedDataUpdated(accessor);
		if (WIDTH.equals(accessor) || HEIGHT.equals(accessor)) {
			this.refreshDimensions();
		}
	}

	@Override
	protected EntityDimensions getDefaultDimensions(Pose pose) {
		return EntityDimensions.scalable(this.entityData.get(WIDTH), this.entityData.get(HEIGHT));
	}

	@Override
	protected void actuallyHurt(ServerLevel level, DamageSource source, float dmg) {
		// Minecraft has applied everything (crit, sharpness, strength, cooldown, invulnerability
		// frames). Hand the result to Skyrim instead of lowering our own health.
		if (this.isInvulnerableTo(level, source) || dmg <= 0.0F) {
			return;
		}
		if (dev.witchercraft.WcOptions.fire && source.is(net.minecraft.world.damagesource.DamageTypes.ON_FIRE)) {
			return; // WitcherCraft: the Witcher's own burning does the after-burn (SkyCombat.fireTick)
		}
		this.pendingDamage += dmg;
		if (source.getDirectEntity() instanceof Projectile) {
			this.pendingFlags |= Proto.HIT_PROJECTILE;
		}
		this.pendingWeapon = weaponClass(source);
		if (source.is(net.minecraft.tags.DamageTypeTags.IS_FIRE)) {
			this.pendingFlags |= Proto.HIT_FIRE;
		}
		if (source.getEntity() instanceof LivingEntity attacker && !(attacker instanceof Player)) {
			this.hitByMob = true;
		} else {
			this.hitByPlayer = true;
		}
		this.hitThisTick = true;
		this.getCombatTracker().recordDamage(source, dmg);
	}

	@Override
	public void knockback(double power, double xd, double zd, DamageSource source, float damage, boolean comesFromEffect) {
		// Skyrim owns this actor's position. Remember the strongest push for Skyrim's stagger:
		// Minecraft pushes towards -(xd, zd).
		double len = Math.sqrt(xd * xd + zd * zd);
		if (len > 1e-6 && power > this.pushStrength) {
			this.pushStrength = (float) power;
			this.pushX = -xd / len;
			this.pushZ = -zd / len;
		}
		this.hitThisTick = true;
	}

	/** Player.crit() was called on us this tick. */
	public void markCritical() {
		this.pendingFlags |= Proto.HIT_CRITICAL;
	}

	/** Which kind of Skyrim weapon impact this hit should look and sound like. */
	private static int weaponClass(DamageSource source) {
		if (source.getDirectEntity() instanceof net.minecraft.world.entity.projectile.arrow.ThrownTrident) {
			return Proto.WEAPON_PIERCE;
		}
		if (source.getDirectEntity() instanceof Projectile) {
			return Proto.WEAPON_ARROW;
		}
		ItemStack weapon = source.getWeaponItem();
		if (weapon == null && source.getEntity() instanceof LivingEntity attacker) {
			weapon = attacker.getMainHandItem();
		}
		if (weapon == null || weapon.isEmpty()) {
			return Proto.WEAPON_UNARMED;
		}
		if (weapon.is(ItemTags.SWORDS)) {
			return Proto.WEAPON_BLADE;
		}
		if (weapon.is(ItemTags.AXES)) {
			return Proto.WEAPON_AXE;
		}
		if (weapon.is(Items.TRIDENT)) {
			return Proto.WEAPON_PIERCE;
		}
		return Proto.WEAPON_BLUNT;
	}

	/** Returns this tick's hit (damage, flags, push, weapon) and clears it; null if nothing hit us. */
	public float[] takeHit() {
		if (!this.hitThisTick) {
			return null;
		}
		int flags = this.pendingFlags | (this.hitByMob && !this.hitByPlayer ? Proto.HIT_BY_MOB : 0);
		float[] hit = { this.pendingDamage, (float) this.pushX, (float) this.pushZ, this.pushStrength, Float.intBitsToFloat(flags),
			Float.intBitsToFloat(this.pendingWeapon) };
		this.pendingDamage = 0.0F;
		this.pendingFlags = 0;
		this.hitByPlayer = this.hitByMob = false;
		this.pushX = this.pushZ = 0.0;
		this.pushStrength = 0.0F;
		this.hitThisTick = false;
		return hit;
	}

	@Override
	public void tick() {
		// Position and rotation come from Skyrim (SkyCombat); keep hurt timers and fire ticking.
		this.baseTick();
		if (this.level() instanceof ServerLevel level) {
			this.fireAndWater(level);
		}
		this.setHealth(this.getMaxHealth());
	}

	/**
	 * WitcherCraft: lava, fire and water the actor stands in. A stand-in has no physics, so Minecraft
	 * never applies the blocks it is in (Entity.isAffectedByBlocks); this does it for those three
	 * only (portals, cactus, berry bushes and the rest stay out of it). Water wins, as in Minecraft.
	 */
	private void fireAndWater(ServerLevel level) {
		AABB box = this.getBoundingBox().deflate(1.0E-5);
		boolean water = false, lava = false, fire = false;
		for (BlockPos pos : BlockPos.betweenClosed(BlockPos.containing(box.minX, box.minY, box.minZ), BlockPos.containing(box.maxX, box.maxY, box.maxZ))) {
			BlockState state = level.getBlockState(pos);
			FluidState fluid = state.getFluidState();
			if (!fluid.isEmpty() && pos.getY() + fluid.getHeight(level, pos) >= box.minY) {
				water |= fluid.is(FluidTags.WATER);
				lava |= fluid.is(FluidTags.LAVA);
			}
			fire |= state.getBlock() instanceof BaseFireBlock || (state.getBlock() instanceof CampfireBlock && state.getValue(CampfireBlock.LIT));
		}
		if (water) {
			if (this.getRemainingFireTicks() > 0 || this.w3Burning) {
				this.doused = true;
			}
			this.clearFire();
		} else if (lava) {
			if (!this.w3NoFire) {
				this.lavaIgnite();
			}
			this.lavaHurt();
		} else if (fire) {
			if (!this.w3NoFire) {
				this.igniteForSeconds(8.0F);
			}
			this.hurtServer(level, this.damageSources().inFire(), 1.0F);
		}
	}

	/** Flint and steel or a fire charge used on a Witcher actor sets it on fire. */
	@Override
	public InteractionResult interact(Player player, InteractionHand hand, Vec3 location) {
		ItemStack stack = player.getItemInHand(hand);
		if (!dev.witchercraft.WcOptions.fire || this.w3NoFire || !(stack.is(Items.FLINT_AND_STEEL) || stack.is(Items.FIRE_CHARGE))) {
			return super.interact(player, hand, location);
		}
		if (this.level() instanceof ServerLevel level) {
			boolean flint = stack.is(Items.FLINT_AND_STEEL);
			level.playSound(null, this.getX(), this.getY(), this.getZ(), flint ? SoundEvents.FLINTANDSTEEL_USE : SoundEvents.FIRECHARGE_USE,
				SoundSource.PLAYERS, 1.0F, level.getRandom().nextFloat() * 0.4F + 0.8F);
			this.igniteForSeconds(8.0F);
			if (flint) {
				stack.hurtAndBreak(1, player, hand.asEquipmentSlot());
			} else {
				stack.consume(1, player);
			}
		}
		return InteractionResult.SUCCESS;
	}

	/** Splash water bottles only look for burning entities (AbstractThrownPotion): count the Witcher's fire. */
	@Override
	public boolean isOnFire() {
		return super.isOnFire() || (this.w3Burning && !this.level().isClientSide());
	}

	@Override
	public void extinguishFire() {
		if (this.isOnFire()) {
			this.doused = true;
		}
		super.extinguishFire();
	}

	public void setW3Burning(boolean burning) {
		this.w3Burning = burning;
	}

	public boolean w3Burning() {
		return this.w3Burning;
	}

	public void setW3NoFire(boolean noFire) {
		this.w3NoFire = noFire;
	}

	public boolean w3NoFire() {
		return this.w3NoFire;
	}

	public void setW3Enemy(boolean enemy) {
		this.w3Enemy = enemy;
	}

	/** Hostile to Geralt and able to take damage: Minecraft mobs fight it (WitcherEnemyTargetGoal). Server side only. */
	public boolean isW3Enemy() {
		return this.w3Enemy;
	}

	/** Mobs don't see friendly or neutral NPCs (or immortal ones) as something to attack, and tamed wolves leave them alone. */
	@Override
	public boolean canBeSeenByAnyone() {
		return this.w3Enemy && super.canBeSeenByAnyone();
	}

	/** Invisible only so Minecraft doesn't draw it: mobs judge the real actor, not a 7 % "invisible" one. */
	@Override
	public double getVisibilityPercent(ServerLevel level, @Nullable Entity targetingEntity) {
		return 1.0;
	}

	/** Water touched us while burning since the last call. */
	public boolean takeDoused() {
		boolean d = this.doused;
		this.doused = false;
		return d;
	}

	public long igniteSentAt() {
		return this.igniteSentAt;
	}

	public void setIgniteSentAt(long gameTime) {
		this.igniteSentAt = gameTime;
	}

	/** The actor table moved the actor (W3 position in MC coordinates). */
	public void syncFromWitcher(double x, double y, double z, long gameTime) {
		if (this.tableTick != Long.MIN_VALUE && gameTime > this.tableTick) {
			double dt = (gameTime - this.tableTick) / 20.0;
			double dx = x - this.tableX, dz = z - this.tableZ;
			if (dt > 0.5 || dx * dx + dz * dz > 25.0) {
				this.velX = this.velZ = 0.0; // stood still for long, or teleported
			} else {
				this.velX = 0.5 * this.velX + 0.5 * net.minecraft.util.Mth.clamp(dx / dt, -MAX_SPEED, MAX_SPEED);
				this.velZ = 0.5 * this.velZ + 0.5 * net.minecraft.util.Mth.clamp(dz / dt, -MAX_SPEED, MAX_SPEED);
			}
		}
		this.tableX = x;
		this.tableY = y;
		this.tableZ = z;
		this.tableTick = gameTime;
		this.setPos(x + this.velX * LEAD_S, y, z + this.velZ * LEAD_S);
	}

	/** Between table updates: keep moving along the actor's velocity for a short while. */
	public void extrapolate(long gameTime) {
		if (this.tableTick == Long.MIN_VALUE || gameTime <= this.tableTick) {
			return;
		}
		double age = Math.min((gameTime - this.tableTick) / 20.0, MAX_EXTRAPOLATE_S);
		this.setPos(this.tableX + this.velX * (LEAD_S + age), this.tableY, this.tableZ + this.velZ * (LEAD_S + age));
	}

	/** A little easier to aim at than its exact box (like Minecraft's small mobs), more so for small ones. */
	@Override
	public float getPickRadius() {
		return this.getBbWidth() < 1.0F ? 0.25F : 0.15F;
	}

	@Override
	public boolean isPushable() {
		return false;
	}

	@Override
	protected void doPush(Entity entity) {
	}

	@Override
	public boolean canBeCollidedWith(@Nullable Entity other) {
		return false;
	}

	@Override
	public boolean shouldShowName() {
		return false;
	}

	@Override
	public boolean shouldBeSaved() {
		return false;
	}

	@Override
	protected @Nullable SoundEvent getHurtSound(DamageSource source) {
		return null; // Skyrim plays the NPC's own pain sounds
	}

	@Override
	protected @Nullable SoundEvent getDeathSound() {
		return null;
	}

	@Override
	public HumanoidArm getMainArm() {
		return HumanoidArm.RIGHT;
	}
}
