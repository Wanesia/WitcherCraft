package dev.witchercraft.combat;

import dev.witchercraft.WitcherCraft;
import dev.witchercraft.link.Proto;
import dev.witchercraft.link.SkyLink;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.fabricmc.fabric.api.object.builder.v1.entity.FabricDefaultAttributeRegistry;
import net.minecraft.core.Registry;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.damagesource.DamageSources;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.MobCategory;
import org.jspecify.annotations.Nullable;

/**
 * Combat between the Minecraft player and Skyrim actors, server side.
 *
 * <p>Every Skyrim actor near the player gets an invisible {@link SkyrimActorEntity} at its exact
 * position. Minecraft weapons hit those like any mob; the resulting damage is sent to Skyrim, which
 * applies it to the real actor (scaled by level) and makes it fight back. Skyrim's hits on the player
 * come back as Minecraft damage from the attacker's stand-in, so armor, shields, knockback, hurt
 * sounds and death all work the Minecraft way.
 */
public final class SkyCombat {
	public static final ResourceKey<EntityType<?>> SKYRIM_ACTOR_KEY =
		ResourceKey.create(Registries.ENTITY_TYPE, Identifier.fromNamespaceAndPath(WitcherCraft.MOD_ID, "skyrim_actor"));
	public static final EntityType<SkyrimActorEntity> SKYRIM_ACTOR = Registry.register(
		BuiltInRegistries.ENTITY_TYPE,
		SKYRIM_ACTOR_KEY,
		EntityType.Builder.<SkyrimActorEntity>of(SkyrimActorEntity::new, MobCategory.MISC)
			.sized(0.6F, 1.8F)
			.noSave()
			.noSummon()
			.noLootTable()
			.clientTrackingRange(10)
			.updateInterval(1)
			.build(SKYRIM_ACTOR_KEY)
	);

	/** Skyrim damage is divided by this for Minecraft (a 15-damage bandit swing = 3 = 1.5 hearts). */
	public static final float SKYRIM_TO_MC_DAMAGE = 5.0F;

	private static final Map<Integer, SkyrimActorEntity> PROXIES = new HashMap<>();
	private static final List<SkyLink.Actor> ACTORS = new ArrayList<>();
	private static int enemyCount;

	private SkyCombat() {
	}

	public static void init() {
		FabricDefaultAttributeRegistry.register(SKYRIM_ACTOR, LivingEntity.createLivingAttributes());
		ServerTickEvents.END_SERVER_TICK.register(SkyCombat::serverTick);
	}

	public static @Nullable SkyrimActorEntity proxy(int formId) {
		return PROXIES.get(formId);
	}

	/** Every Witcher actor's stand-in (server thread only). */
	public static Iterable<SkyrimActorEntity> proxies() {
		return PROXIES.values();
	}

	/** Any stand-in Minecraft mobs may fight (WitcherEnemyTargetGoal), as of the last actor table. */
	public static boolean hasEnemies() {
		return enemyCount > 0;
	}

	private static void serverTick(MinecraftServer server) {
		List<ServerPlayer> players = server.getPlayerList().getPlayers();
		if (!SkyLink.active() || players.isEmpty()) {
			removeAll();
			return;
		}
		ServerLevel level = players.getFirst().level();
		for (ServerPlayer player : players) {
			pickUpNearby(player);
		}
		if (SkyLink.readActors(ACTORS)) {
			sync(level);
			WcBossBars.update(ACTORS, players);
		} else {
			long now = level.getGameTime();
			for (SkyrimActorEntity proxy : PROXIES.values()) {
				proxy.extrapolate(now); // where the Witcher draws it now (hit boxes follow running actors)
			}
		}
		// Hits land during the tick (melee, sweeps, arrows, fire); send one combined hit per actor.
		for (SkyrimActorEntity proxy : PROXIES.values()) {
			float[] hit = proxy.takeHit();
			if (hit != null && (hit[0] > 0.0F || hit[3] > 0.0F)) {
				SkyLink.pushEvent(
					Proto.EV_HIT_ACTOR, proxy.formId(), hit[0], hit[1], hit[2], hit[3], Float.floatToRawIntBits(hit[4]), Float.floatToRawIntBits(hit[5])
				);
				WitcherCraft.LOG.info("WitcherCraft: hit {} for {} (knockback {}){}", proxy.getName().getString(), hit[0], hit[3],
					(Float.floatToRawIntBits(hit[4]) & Proto.HIT_BY_MOB) != 0 ? " by a Minecraft mob" : "");
			}
			fireTick(level, proxy);
		}
	}

	/**
	 * WitcherCraft: Minecraft fire on a stand-in (lava, fire, flint and steel, fire aspect, flame
	 * arrows, fire charges) sets the Witcher actor burning, the Witcher's way: it flails, panics and
	 * takes the Witcher's fire damage (so Minecraft's after-burn isn't forwarded too; fire-proof
	 * monsters refuse it). Water (standing in it, a bucket poured on it, a splash water bottle) puts
	 * out either fire. Re-sent once a second while Minecraft's fire outlasts the Witcher's.
	 */
	private static void fireTick(ServerLevel level, SkyrimActorEntity proxy) {
		long now = level.getGameTime();
		boolean doused = proxy.takeDoused();
		if (doused && dev.witchercraft.WcOptions.waterDouse && (proxy.w3Burning() || now - proxy.igniteSentAt() < 40)) {
			SkyLink.pushEvent(Proto.EV_ACTOR_FIRE, proxy.formId(), 0.0F, 0.0F, 0.0F, 0.0F, 0);
			proxy.setIgniteSentAt(Long.MIN_VALUE / 2);
			proxy.setW3Burning(false); // until the actor table says otherwise
			WitcherCraft.LOG.info("WitcherCraft: water put out {}", proxy.getName().getString());
			return;
		}
		int ticks = proxy.getRemainingFireTicks();
		if (dev.witchercraft.WcOptions.fire && ticks > 0 && proxy.w3NoFire()) {
			// The Witcher won't burn it (a crowd NPC with no health, fire-proof, friendly to Geralt):
			// no Minecraft flames on it either (Fire Aspect, Flame arrows), and no retry every second.
			proxy.clearFire();
			return;
		}
		if (dev.witchercraft.WcOptions.fire && ticks > 0 && !proxy.w3Burning() && now - proxy.igniteSentAt() >= 20) {
			SkyLink.pushEvent(Proto.EV_ACTOR_FIRE, proxy.formId(), ticks / 20.0F, 0.0F, 0.0F, 0.0F, 0);
			proxy.setIgniteSentAt(now);
			WitcherCraft.LOG.info("WitcherCraft: set fire to {} for {} s", proxy.getName().getString(), Math.round(ticks / 20.0F));
		}
	}

	private static void sync(ServerLevel level) {
		Map<Integer, SkyLink.Actor> live = new HashMap<>();
		for (SkyLink.Actor a : ACTORS) {
			if (!a.dead()) {
				live.put(a.formId(), a);
			}
		}
		for (Iterator<Map.Entry<Integer, SkyrimActorEntity>> it = PROXIES.entrySet().iterator(); it.hasNext(); ) {
			Map.Entry<Integer, SkyrimActorEntity> e = it.next();
			SkyrimActorEntity proxy = e.getValue();
			if (!live.containsKey(e.getKey()) || proxy.isRemoved() || proxy.level() != level) {
				proxy.discard();
				it.remove();
			}
		}
		int before = PROXIES.size();
		enemyCount = 0;
		for (SkyLink.Actor a : live.values()) {
			SkyrimActorEntity proxy = PROXIES.get(a.formId());
			if (proxy == null) {
				proxy = new SkyrimActorEntity(SKYRIM_ACTOR, level);
				proxy.setFormId(a.formId());
				proxy.setSize(a.width(), a.height());
				proxy.snapTo(a.x(), a.y(), a.z(), a.yaw(), 0.0F);
				proxy.syncFromWitcher(a.x(), a.y(), a.z(), level.getGameTime());
				proxy.setW3Burning(a.burning());
				proxy.setW3NoFire((a.flags() & Proto.ACTOR_NO_FIRE) != 0);
				proxy.setW3Enemy(isEnemy(a));
				if (!a.name().isEmpty()) {
					proxy.setCustomName(Component.literal(a.name()));
				}
				if (!level.addFreshEntity(proxy)) {
					continue;
				}
				PROXIES.put(a.formId(), proxy);
				enemyCount += proxy.isW3Enemy() ? 1 : 0;
				continue;
			}
			proxy.setSize(a.width(), a.height());
			proxy.syncFromWitcher(a.x(), a.y(), a.z(), level.getGameTime());
			proxy.setYRot(a.yaw());
			proxy.setYHeadRot(a.yaw());
			proxy.setW3Burning(a.burning());
			proxy.setW3NoFire((a.flags() & Proto.ACTOR_NO_FIRE) != 0);
			proxy.setW3Enemy(isEnemy(a));
			enemyCount += proxy.isW3Enemy() ? 1 : 0;
			stepOnTriggers(level, proxy);
		}
		if (PROXIES.size() != before && (PROXIES.size() % 5 == 0 || PROXIES.size() < 5)) {
			WitcherCraft.LOG.info("WitcherCraft: {} Witcher actors mirrored as hittable stand-ins", PROXIES.size());
		}
	}

	/**
	 * WitcherCraft: an actor Minecraft mobs fight: hostile to Geralt and able to take damage (not
	 * immortal or invulnerable right now, and not a crowd NPC without health).
	 */
	private static boolean isEnemy(SkyLink.Actor a) {
		return a.hostile() && (a.flags() & Proto.ACTOR_ESSENTIAL) == 0 && a.healthFrac() > 0.0F;
	}

	/**
	 * Skyrim's NPCs press pressure plates and trip tripwires. Their stand-ins are placed, not moved
	 * (no physics), so Minecraft never checks what they step into; do it for those blocks here.
	 */
	private static void stepOnTriggers(ServerLevel level, SkyrimActorEntity proxy) {
		var box = proxy.getBoundingBox().deflate(1.0E-5);
		var from = net.minecraft.core.BlockPos.containing(box.minX, box.minY, box.minZ);
		var to = net.minecraft.core.BlockPos.containing(box.maxX, box.maxY, box.maxZ);
		for (var pos : net.minecraft.core.BlockPos.betweenClosed(from, to)) {
			var state = level.getBlockState(pos);
			if (state.getBlock() instanceof net.minecraft.world.level.block.BasePressurePlateBlock
				|| state.getBlock() instanceof net.minecraft.world.level.block.TripWireBlock) {
				state.entityInside(level, pos, proxy, net.minecraft.world.entity.InsideBlockEffectApplier.NOOP, true);
			}
		}
	}

	/**
	 * Items and stuck arrows on Skyrim ground rest on its collision voxels, which on steep or rough
	 * terrain can sit a little off from where the player (on Skyrim's exact triangles) stands.
	 * Touch them over a slightly bigger area than vanilla's so walking over them picks them up.
	 * playerTouch applies all of Minecraft's own rules (pickup delay, owner, inventory space).
	 */
	private static void pickUpNearby(ServerPlayer player) {
		if (!player.isAlive() || player.isSpectator()) {
			return;
		}
		for (Entity entity : player.level().getEntities(player, player.getBoundingBox().inflate(1.25, 1.0, 1.25))) {
			if (!entity.isRemoved() && (entity instanceof net.minecraft.world.entity.item.ItemEntity
				|| entity instanceof net.minecraft.world.entity.projectile.arrow.AbstractArrow)) {
				entity.playerTouch(player);
			}
		}
	}

	private static void removeAll() {
		WcBossBars.clear();
		enemyCount = 0;
		if (PROXIES.isEmpty()) {
			return;
		}
		PROXIES.values().forEach(Entity::discard);
		PROXIES.clear();
	}

	/**
	 * Skyrim hit the player. Runs on the server thread. {@code kind} is a Proto.HURT_* value and
	 * {@code skyrimDamage} is what Skyrim would have taken off the player's health.
	 */
	public static void hurtPlayer(ServerPlayer player, int kind, float skyrimDamage, int attackerFormId, int flags) {
		if (!player.isAlive() || skyrimDamage <= 0.0F) {
			return;
		}
		ServerLevel level = player.level();
		SkyrimActorEntity attacker = PROXIES.get(attackerFormId);
		if (attacker != null && attacker.distanceToSqr(player) > 24.0 * 24.0) {
			attacker = null; // a guest's own NPC with the same form id as one of the host's
		}
		if (kind == Proto.HURT_MELEE && attacker != null && outOfMeleeReach(attacker, player)) {
			// The Witcher fights the hidden Geralt, who stays on its ground under Steve: an enemy
			// down there swinging at him can't reach Steve on top of a pillar (as a Minecraft zombie
			// can't hit a player standing above its head). Arrows and spells still land.
			WitcherCraft.LOG.info("WitcherCraft: The Witcher's melee hit from {} out of reach ({} m below, {} m away): ignored",
				attacker.getName().getString(), String.format("%.1f", player.getY() - attacker.getBoundingBox().maxY),
				String.format("%.1f", Math.hypot(player.getX() - attacker.getX(), player.getZ() - attacker.getZ())));
			return;
		}
		DamageSources sources = level.damageSources();
		DamageSource source = switch (kind) {
			case Proto.HURT_MELEE -> attacker != null ? sources.mobAttack(attacker) : sources.generic();
			case Proto.HURT_PROJECTILE -> attacker != null ? sources.mobProjectile(attacker, attacker) : sources.generic();
			case Proto.HURT_MAGIC -> attacker != null ? sources.indirectMagic(attacker, attacker) : sources.magic();
			default -> sources.generic();
		};
		float damage = skyrimDamage / SKYRIM_TO_MC_DAMAGE;
		float healthBefore = player.getHealth();
		boolean blocking = player.isBlocking();
		boolean hurt = player.hurtServer(level, source, damage);
		trainDefence(player, damage, blocking && player.getHealth() >= healthBefore - 1.0E-3F);
		WitcherCraft.LOG.info("WitcherCraft: The Witcher hit the player for {} ({} Minecraft): health {} -> {}{}", skyrimDamage, damage, healthBefore, player.getHealth(),
			hurt ? "" : " (blocked/immune)");
		if (hurt && attacker != null && (flags & Proto.HURT_POWER_ATTACK) != 0 && !player.isBlocking()) {
			// Power attacks shove harder, like a sprint hit does in Minecraft.
			player.knockback(0.5, attacker.getX() - player.getX(), attacker.getZ() - player.getZ(), source, damage);
		}
	}

	/** Melee reach: Steve's feet more than 1 block over the attacker's head, or 6 blocks away from it (big monsters reach far). */
	static boolean outOfMeleeReach(Entity attacker, Entity player) {
		double above = player.getY() - attacker.getBoundingBox().maxY;
		double away = Math.hypot(player.getX() - attacker.getX(), player.getZ() - attacker.getZ());
		return above > 1.0 || away > 6.0;
	}

	/**
	 * Skyrim skills for taking a hit: Block when the shield caught it, otherwise Light or Heavy
	 * Armor by what the player mostly wears (leather, chainmail, gold, copper and turtle count as
	 * light; iron, diamond and netherite as heavy). Only the host's own Skyrim is told.
	 */
	private static void trainDefence(ServerPlayer player, float damage, boolean blocked) {
		if (!dev.witchercraft.net.SkyNet.isHost(player) || damage <= 0.0F) {
			return;
		}
		if (blocked) {
			SkyLink.pushEvent(Proto.EV_SKILL_USE, Proto.SKILL_BLOCK, damage, 0.0F, 0.0F, 0.0F, 0);
			return;
		}
		int light = 0, heavy = 0;
		for (var slot : new net.minecraft.world.entity.EquipmentSlot[] { net.minecraft.world.entity.EquipmentSlot.HEAD, net.minecraft.world.entity.EquipmentSlot.CHEST,
			net.minecraft.world.entity.EquipmentSlot.LEGS, net.minecraft.world.entity.EquipmentSlot.FEET }) {
			var stack = player.getItemBySlot(slot);
			if (stack.isEmpty()) {
				continue;
			}
			String path = net.minecraft.core.registries.BuiltInRegistries.ITEM.getKey(stack.getItem()).getPath();
			if (path.startsWith("iron_") || path.startsWith("diamond_") || path.startsWith("netherite_")) {
				heavy++;
			} else {
				light++;
			}
		}
		if (light + heavy > 0) {
			SkyLink.pushEvent(Proto.EV_SKILL_USE, heavy > light ? Proto.SKILL_HEAVY_ARMOR : Proto.SKILL_LIGHT_ARMOR, damage * (light + heavy) / 4.0F, 0.0F, 0.0F,
				0.0F, 0);
		}
	}

	/** Form id of the Skyrim actor behind a damage source, or 0. */
	public static int attackerFormId(DamageSource source) {
		return source.getEntity() instanceof SkyrimActorEntity proxy ? proxy.formId() : 0;
	}
}
