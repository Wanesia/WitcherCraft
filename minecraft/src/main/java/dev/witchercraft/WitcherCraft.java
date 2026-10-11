package dev.witchercraft;

import dev.witchercraft.combat.SkyCombat;
import net.fabricmc.api.ModInitializer;
import net.minecraft.world.entity.EquipmentSlot;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerLifecycleEvents;
import net.fabricmc.fabric.api.networking.v1.ServerPlayConnectionEvents;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.level.gamerules.GameRules;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

public final class WitcherCraft implements ModInitializer {
	public static final String MOD_ID = "witchercraft";
	public static final String WORLD_NAME = "WitcherCraft";
	public static final Logger LOG = LoggerFactory.getLogger(MOD_ID);
	private static final String KIT2_TAG = "witchercraft_builder_kit";
	// v2: the hotbar kit is handed out once more to players who had v1.
	private static final String HOTBAR_TAG = "witchercraft_hotbar_v2";
	private static int appliedOptionsVersion = -1;

	@Override
	public void onInitialize() {
		SkyCombat.init();
		dev.witchercraft.net.SkyNet.init();
		dev.witchercraft.world.SkyDig.init();
		ServerLifecycleEvents.SERVER_STARTED.register(WitcherCraft::configureServer);
		ServerPlayConnectionEvents.JOIN.register((handler, sender, server) -> {
			giveStarterKit(handler.getPlayer());
			giveBuilderKit(handler.getPlayer());
			giveWitcherHotbar(handler.getPlayer());
			giveFlightKit(handler.getPlayer());
			dressTestGuest(handler.getPlayer());
			applySpeed(handler.getPlayer());
		});
		// WitcherCraft options (WcOptions): Steve's speed when it changes; no hunger (off by default).
		net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents.END_SERVER_TICK.register(server -> {
			boolean changed = appliedOptionsVersion != WcOptions.version();
			appliedOptionsVersion = WcOptions.version();
			if (!changed && server.getTickCount() % 20 != 0) {
				return;
			}
			for (ServerPlayer p : server.getPlayerList().getPlayers()) {
				if (changed) {
					applySpeed(p);
				}
				if (!WcOptions.hunger && !p.isCreative() && p.getFoodData().getFoodLevel() < 17) {
					p.getFoodData().setFoodLevel(17); // sprinting needs > 6; natural healing needs 18 (food still heals)
				}
			}
		});
		// An old offline test (overlay_test) left a frozen pig near the world origin, where a new
		// player waits while The Witcher loads ("random pigs"): such test pigs are removed on load.
		net.fabricmc.fabric.api.event.lifecycle.v1.ServerEntityEvents.ENTITY_LOAD.register((entity, level) -> {
			if (entity.getType() == net.minecraft.world.entity.EntityTypes.PIG && entity instanceof net.minecraft.world.entity.Mob mob && mob.isNoAi()) {
				LOG.info("WitcherCraft: removed a leftover test pig at {}", entity.blockPosition());
				entity.discard();
			} else if (WcRoach.isStale(entity, level)) {
				LOG.info("WitcherCraft: removed an old copy of Roach at {}", entity.blockPosition());
				entity.discard();
			}
		});
	}

	/** Steve's walking/sprinting speed from the options (100% = vanilla 0.1); test players keep theirs. */
	private static void applySpeed(ServerPlayer player) {
		if (player.getName().getString().startsWith("WcTest")) {
			return;
		}
		var attr = player.getAttribute(net.minecraft.world.entity.ai.attributes.Attributes.MOVEMENT_SPEED);
		double want = 0.1 * WcOptions.speedPct / 100.0;
		if (attr != null && Math.abs(attr.getBaseValue() - want) > 1e-6) {
			attr.setBaseValue(want);
			LOG.info("WitcherCraft: {}'s speed {}%", player.getName().getString(), WcOptions.speedPct);
		}
	}

	/** The mirror world is a void that only exists to host the player; Skyrim drives time and spawning. */
	private static void configureServer(MinecraftServer server) {
		GameRules rules = server.getGameRules();
		rules.set(GameRules.ADVANCE_TIME, false, server);
		rules.set(GameRules.ADVANCE_WEATHER, false, server);
		rules.set(GameRules.SPAWN_MOBS, false, server);
		rules.set(GameRules.SPAWN_MONSTERS, false, server);
		rules.set(GameRules.SPAWN_PHANTOMS, false, server);
		rules.set(GameRules.SPAWN_PATROLS, false, server);
		rules.set(GameRules.SPAWN_WANDERING_TRADERS, false, server);
		rules.set(GameRules.PLAYER_MOVEMENT_CHECK, false, server);
		rules.set(GameRules.KEEP_INVENTORY, true, server);
		rules.set(GameRules.IMMEDIATE_RESPAWN, true, server);
		rules.set(GameRules.SHOW_ADVANCEMENT_MESSAGES, false, server);
		server.getCommands().performPrefixedCommand(server.createCommandSourceStack().withSuppressedOutput(), "time set noon");
		LOG.info("WitcherCraft: mirror world configured");
	}

	/**
	 * Local multiplayer test guests (tools/fake_guest.py; named Guest, Guest2, ...) wear a random
	 * mix of iron and diamond armour, so they're easy to tell apart.
	 */
	private static void dressTestGuest(ServerPlayer player) {
		if (!player.getName().getString().startsWith("Guest")) {
			return;
		}
		var random = player.getRandom();
		EquipmentSlot[] slots = { EquipmentSlot.HEAD, EquipmentSlot.CHEST, EquipmentSlot.LEGS, EquipmentSlot.FEET };
		net.minecraft.world.item.Item[][] pieces = {
			{ Items.IRON_HELMET, Items.DIAMOND_HELMET },
			{ Items.IRON_CHESTPLATE, Items.DIAMOND_CHESTPLATE },
			{ Items.IRON_LEGGINGS, Items.DIAMOND_LEGGINGS },
			{ Items.IRON_BOOTS, Items.DIAMOND_BOOTS },
		};
		for (int i = 0; i < slots.length; i++) {
			player.setItemSlot(slots[i], new ItemStack(pieces[i][random.nextBoolean() ? 1 : 0]));
		}
		LOG.info("WitcherCraft: dressed test guest {} in iron and diamond", player.getName().getString());
	}

	private static void giveStarterKit(ServerPlayer player) {
		if (!player.getInventory().isEmpty()) {
			return;
		}
		player.getInventory().add(new ItemStack(Items.DIAMOND_SWORD));
		player.getInventory().add(new ItemStack(Items.DIAMOND_PICKAXE));
		player.getInventory().add(new ItemStack(Items.BOW));
		player.getInventory().add(new ItemStack(Items.COOKED_BEEF, 32));
		player.getInventory().add(new ItemStack(Items.OAK_PLANKS, 64));
		player.getInventory().add(new ItemStack(Items.TORCH, 32));
		player.getInventory().add(new ItemStack(Items.ARROW, 64));
		player.setItemSlot(net.minecraft.world.entity.EquipmentSlot.OFFHAND, new ItemStack(Items.SHIELD));
		LOG.info("WitcherCraft: gave starter kit to {}", player.getName().getString());
	}

	/**
	 * Once per player: armor (Skyrim's enemies hit back now) and building materials, since there is
	 * no Minecraft terrain to mine in Skyrim.
	 */
	private static void giveBuilderKit(ServerPlayer player) {
		if (player.entityTags().contains(KIT2_TAG)) {
			return;
		}
		equipIfEmpty(player, EquipmentSlot.HEAD, Items.IRON_HELMET);
		equipIfEmpty(player, EquipmentSlot.CHEST, Items.IRON_CHESTPLATE);
		equipIfEmpty(player, EquipmentSlot.LEGS, Items.IRON_LEGGINGS);
		equipIfEmpty(player, EquipmentSlot.FEET, Items.IRON_BOOTS);
		var inventory = player.getInventory();
		inventory.add(new ItemStack(Items.COBBLESTONE, 64));
		inventory.add(new ItemStack(Items.STONE_BRICKS, 64));
		inventory.add(new ItemStack(Items.OAK_LOG, 64));
		inventory.add(new ItemStack(Items.GLASS, 64));
		inventory.add(new ItemStack(Items.OAK_STAIRS, 64));
		inventory.add(new ItemStack(Items.OAK_SLAB, 64));
		inventory.add(new ItemStack(Items.OAK_DOOR, 8));
		inventory.add(new ItemStack(Items.LADDER, 32));
		inventory.add(new ItemStack(Items.LANTERN, 16));
		inventory.add(new ItemStack(Items.CRAFTING_TABLE));
		inventory.add(new ItemStack(Items.WATER_BUCKET));
		inventory.add(new ItemStack(Items.ARROW, 64));
		inventory.add(new ItemStack(Items.GOLDEN_APPLE, 4));
		player.addTag(KIT2_TAG);
		LOG.info("WitcherCraft: gave builder kit to {}", player.getName().getString());
	}

	/**
	 * WitcherCraft, once per player: keys 1-9 hold sword, pickaxe, bow,
	 * golden apples, ender pearls, oak logs, cobblestone, lava, water; a shield in the off hand.
	 * Whatever was in those slots moves to the inventory.
	 */
	private static void giveWitcherHotbar(ServerPlayer player) {
		if (player.entityTags().contains(HOTBAR_TAG)) {
			return;
		}
		var inventory = player.getInventory();
		ItemStack[] bar = {
			new ItemStack(Items.DIAMOND_SWORD), new ItemStack(Items.DIAMOND_PICKAXE), new ItemStack(Items.BOW),
			new ItemStack(Items.GOLDEN_APPLE, 64), new ItemStack(Items.ENDER_PEARL, 16), new ItemStack(Items.OAK_LOG, 64),
			new ItemStack(Items.COBBLESTONE, 64), new ItemStack(Items.LAVA_BUCKET), new ItemStack(Items.WATER_BUCKET),
		};
		java.util.List<ItemStack> moved = new java.util.ArrayList<>();
		for (int i = 0; i < bar.length; i++) {
			ItemStack old = inventory.getItem(i);
			if (!old.isEmpty()) {
				moved.add(old);
			}
			inventory.setItem(i, bar[i]);
		}
		for (ItemStack old : moved) {
			inventory.add(old); // full inventory: the leftover kit duplicate is dropped
		}
		ItemStack offhand = player.getItemBySlot(EquipmentSlot.OFFHAND);
		if (!offhand.is(Items.SHIELD)) {
			if (!offhand.isEmpty()) {
				inventory.add(offhand);
			}
			player.setItemSlot(EquipmentSlot.OFFHAND, new ItemStack(Items.SHIELD));
		}
		if (inventory.countItem(Items.ARROW) == 0) {
			inventory.add(new ItemStack(Items.ARROW, 64)); // for the bow
		}
		player.addTag(HOTBAR_TAG);
		LOG.info("WitcherCraft: gave {} the hotbar kit (sword, pickaxe, bow, apples, pearls, logs, cobblestone, lava, water; shield)",
			player.getName().getString());
	}

	/**
	 * Every join: the elytra worn and firework rockets in the inventory (not the hotbar: keys 1-9
	 * are the hotbar kit). Replaces the native typed commands, which flashed the chat screen.
	 */
	private static void giveFlightKit(ServerPlayer player) {
		ItemStack chest = player.getItemBySlot(EquipmentSlot.CHEST);
		if (!chest.is(Items.ELYTRA)) {
			if (!chest.isEmpty()) {
				player.getInventory().add(chest);
			}
			player.setItemSlot(EquipmentSlot.CHEST, new ItemStack(Items.ELYTRA));
		}
		if (player.getInventory().countItem(Items.FIREWORK_ROCKET) == 0) {
			player.getInventory().add(new ItemStack(Items.FIREWORK_ROCKET, 64));
		}
	}

	private static void equipIfEmpty(ServerPlayer player, EquipmentSlot slot, net.minecraft.world.item.Item item) {
		if (player.getItemBySlot(slot).isEmpty()) {
			player.setItemSlot(slot, new ItemStack(item));
		} else {
			player.getInventory().add(new ItemStack(item));
		}
	}
}
