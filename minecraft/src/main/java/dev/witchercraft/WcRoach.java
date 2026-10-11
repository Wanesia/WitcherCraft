package dev.witchercraft;

import java.util.UUID;
import net.minecraft.network.chat.Component;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EntitySpawnReason;
import net.minecraft.world.entity.EntityTypes;
import net.minecraft.world.entity.EquipmentSlot;
import net.minecraft.world.entity.ai.attributes.Attributes;
import net.minecraft.world.entity.animal.equine.Horse;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;

/**
 * WitcherCraft: Steve's Roach, a real Minecraft horse (Minecraft's riding controls). The "Call
 * Roach" key (X) brings her next to Steve, or creates her (tame, saddled, chestnut, fast). One
 * Roach per player: her UUID is kept as a player tag; any other Roach that loads is removed.
 * Geralt (Witcher mode) still has the Witcher's own Roach.
 */
public final class WcRoach {
	public static final String HORSE_TAG = "witchercraft_roach";
	private static final String PLAYER_TAG_PREFIX = "witchercraft_roach:";

	private WcRoach() {
	}

	private static UUID storedId(ServerPlayer player) {
		for (String t : player.entityTags()) {
			if (t.startsWith(PLAYER_TAG_PREFIX)) {
				try {
					return UUID.fromString(t.substring(PLAYER_TAG_PREFIX.length()));
				} catch (IllegalArgumentException e) {
					return null;
				}
			}
		}
		return null;
	}

	private static void store(ServerPlayer player, UUID id) {
		player.entityTags().removeIf(t -> t.startsWith(PLAYER_TAG_PREFIX));
		player.addTag(PLAYER_TAG_PREFIX + id);
	}

	/** Server thread. */
	public static void call(ServerPlayer player) {
		if (!(player.level() instanceof ServerLevel level) || player.isPassenger()) {
			return;
		}
		Horse horse = null;
		UUID id = storedId(player);
		if (id != null && level.getEntity(id) instanceof Horse h && h.isAlive()) {
			horse = h;
		}
		boolean fresh = horse == null;
		if (fresh) {
			horse = EntityTypes.HORSE.create(level, EntitySpawnReason.TRIGGERED);
			if (horse == null) {
				return;
			}
			horse.tameWithName(player);
			horse.setItemSlot(EquipmentSlot.SADDLE, new ItemStack(Items.SADDLE));
			horse.setCustomName(Component.literal("Roach"));
			horse.addTag(HORSE_TAG);
			((dev.witchercraft.mixin.HorseInvoker) horse).witchercraft$setVariantAndMarkings(net.minecraft.world.entity.animal.equine.Variant.CHESTNUT, net.minecraft.world.entity.animal.equine.Markings.NONE);
			// A good horse: fast, a decent jump, sturdy.
			var speed = horse.getAttribute(Attributes.MOVEMENT_SPEED);
			if (speed != null) {
				speed.setBaseValue(0.3);
			}
			var jump = horse.getAttribute(Attributes.JUMP_STRENGTH);
			if (jump != null) {
				jump.setBaseValue(0.9);
			}
			var hp = horse.getAttribute(Attributes.MAX_HEALTH);
			if (hp != null) {
				hp.setBaseValue(30.0);
			}
			horse.setHealth(horse.getMaxHealth());
		}
		// Two blocks to Steve's right, facing where he looks.
		double yaw = Math.toRadians(player.getYRot());
		double rx = -Math.cos(yaw), rz = -Math.sin(yaw);
		double x = player.getX() + rx * 2.0, z = player.getZ() + rz * 2.0;
		horse.snapTo(x, player.getY(), z, player.getYRot(), 0.0F);
		horse.setDeltaMovement(net.minecraft.world.phys.Vec3.ZERO);
		if (fresh) {
			store(player, horse.getUUID()); // first: adding her fires ENTITY_LOAD, which removes stale copies
			level.addFreshEntity(horse);
		}
		WitcherCraft.LOG.info("WitcherCraft: Roach {} next to {}", fresh ? "created" : "called", player.getName().getString());
	}

	/** On entity load: a Roach that isn't the current one of any online player is an old copy. */
	public static boolean isStale(Entity entity, ServerLevel level) {
		if (!(entity instanceof Horse) || !entity.entityTags().contains(HORSE_TAG)) {
			return false;
		}
		for (ServerPlayer p : level.getServer().getPlayerList().getPlayers()) {
			if (entity.getUUID().equals(storedId(p))) {
				return false;
			}
		}
		return !level.getServer().getPlayerList().getPlayers().isEmpty();
	}
}
