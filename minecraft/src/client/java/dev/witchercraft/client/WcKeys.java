package dev.witchercraft.client;

import com.mojang.blaze3d.platform.InputConstants;
import net.fabricmc.fabric.api.client.keymapping.v1.KeyMappingHelper;
import net.minecraft.client.KeyMapping;
import net.minecraft.client.Minecraft;
import net.minecraft.resources.Identifier;

/** WitcherCraft key bindings (Options -> Controls -> Key Binds -> WitcherCraft). */
public final class WcKeys {
	private static final KeyMapping.Category CATEGORY = KeyMapping.Category.register(Identifier.fromNamespaceAndPath("witchercraft", "keys"));
	/** Hold for the Witcher's senses (focus mode); the host reads it from McState wcFlags. */
	public static KeyMapping senses;
	/** Calls Steve's Roach (a Minecraft horse; WcRoach). */
	public static KeyMapping roach;

	private WcKeys() {
	}

	public static void register() {
		senses = KeyMappingHelper.registerKeyMapping(new KeyMapping("key.witchercraft.senses", InputConstants.Type.KEYBOARD, InputConstants.KEY_V, CATEGORY));
		roach = KeyMappingHelper.registerKeyMapping(new KeyMapping("key.witchercraft.roach", InputConstants.Type.KEYBOARD, InputConstants.KEY_X, CATEGORY));
	}

	/** Witcher senses and Call Roach back to V and X (Minecraft saves its key bindings). */
	public static void resetToDefaults(Minecraft minecraft) {
		for (KeyMapping k : new KeyMapping[] {senses, roach}) {
			if (k != null) {
				k.setKey(k.getDefaultKey());
			}
		}
		KeyMapping.resetMapping();
		minecraft.options.save();
	}

	/** "Turn all extras off": Witcher senses and Call Roach unbound. */
	public static void unbindExtras(Minecraft minecraft) {
		for (KeyMapping k : new KeyMapping[] {senses, roach}) {
			if (k != null) {
				k.setKey(InputConstants.UNKNOWN);
			}
		}
		KeyMapping.resetMapping();
		minecraft.options.save();
	}

	public static boolean sensesHeld() {
		return senses != null && senses.isDown();
	}

	/** End of each client tick. */
	public static void tick(Minecraft minecraft) {
		if (roach == null) {
			return;
		}
		while (roach.consumeClick()) {
			var server = minecraft.getSingleplayerServer();
			var player = minecraft.player;
			if (server == null || player == null) {
				continue;
			}
			java.util.UUID id = player.getUUID();
			server.execute(() -> {
				var sp = server.getPlayerList().getPlayer(id);
				if (sp != null) {
					dev.witchercraft.WcRoach.call(sp);
				}
			});
		}
	}
}
