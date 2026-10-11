package dev.witchercraft.client;

import dev.witchercraft.WcOptions;
import net.minecraft.client.KeyMapping;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.components.toasts.SystemToast;
import net.minecraft.client.input.KeyEvent;
import net.minecraft.network.chat.Component;

/**
 * Tips for keys whose WitcherCraft feature is turned off: with "Panel keys" off (also by "Turn all
 * extras off"), M/J/I/K/N reach Minecraft as ordinary keys and do nothing, which looks broken to a
 * player who doesn't know the option. A toast (top right) says what the key would do and where to turn
 * it on. Option "Tips for turned-off keys" (WcOptions.offKeyTips) turns the tips off.
 */
public final class WcTips {
	// SDL scancodes (USB HID usage ids) of the Witcher's panel keys, as InputBridge delivers them.
	private static final int SC_I = 12, SC_J = 13, SC_K = 14, SC_M = 16, SC_N = 17;
	private static final SystemToast.SystemToastId PANEL_OFF = new SystemToast.SystemToastId(6000L);

	private WcTips() {
	}

	/** A key went down for Minecraft (first press, not a repeat). */
	static void keyDown(Minecraft minecraft, int scancode, KeyEvent event) {
		if (WcOptions.panelKeys || !WcOptions.offKeyTips || minecraft.player == null || minecraft.gui.screen() != null) {
			return;
		}
		String panel = switch (scancode) {
			case SC_M -> "M: the Witcher's map";
			case SC_J -> "J: the Witcher's journal";
			case SC_I -> "I: the Witcher's inventory";
			case SC_K -> "K: the Witcher's character panel";
			case SC_N -> "N: meditation";
			default -> null;
		};
		if (panel == null || boundInMinecraft(minecraft, event)) {
			return; // not a panel key, or the player gave it a Minecraft action: it isn't "doing nothing"
		}
		show(minecraft, panel);
	}

	static void show(Minecraft minecraft, String panel) {
		SystemToast.addOrUpdate(minecraft.gui.toastManager(), PANEL_OFF, Component.literal(panel + " is turned off"),
			Component.literal("Esc > WitcherCraft... > Options > Panel keys (tips can be turned off there)"));
	}

	private static boolean boundInMinecraft(Minecraft minecraft, KeyEvent event) {
		for (KeyMapping mapping : minecraft.options.keyMappings) {
			if (!mapping.isUnbound() && mapping.matches(event)) {
				return true;
			}
		}
		return false;
	}
}
