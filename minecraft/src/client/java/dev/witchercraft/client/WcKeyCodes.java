package dev.witchercraft.client;

import com.mojang.blaze3d.platform.InputConstants;
import dev.witchercraft.WcOptions;
import net.minecraft.client.KeyMapping;
import net.minecraft.network.chat.Component;

/**
 * Minecraft (GLFW) key codes <-> the DirectInput codes the Witcher side's input hook sees
 * (control.cpp: scan code, | 0x80 for E0 keys). The keys the host handles (interact, the Steve /
 * Geralt switch) are stored as DIK; Minecraft's key bindings are sent as DIK for the key hints.
 */
public final class WcKeyCodes {
	private static final int[] GLFW_TO_DIK = new int[349];
	private static final int[] DIK_TO_GLFW = new int[256];

	static {
		// letters
		int[] letters = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F,
			0x11, 0x2D, 0x15, 0x2C};
		for (int i = 0; i < 26; i++) {
			map(InputConstants.KEY_A + i, letters[i]);
		}
		// digits 1-9, 0
		for (int i = 1; i <= 9; i++) {
			map(InputConstants.KEY_0 + i, 0x01 + i);
		}
		map(InputConstants.KEY_0, 0x0B);
		// F1-F10, F11, F12
		for (int i = 0; i < 10; i++) {
			map(InputConstants.KEY_F1 + i, 0x3B + i);
		}
		map(InputConstants.KEY_F11, 0x57);
		map(InputConstants.KEY_F12, 0x58);
		map(InputConstants.KEY_SPACE, 0x39);
		map(InputConstants.KEY_APOSTROPHE, 0x28);
		map(InputConstants.KEY_COMMA, 0x33);
		map(InputConstants.KEY_MINUS, 0x0C);
		map(InputConstants.KEY_PERIOD, 0x34);
		map(InputConstants.KEY_SLASH, 0x35);
		map(InputConstants.KEY_SEMICOLON, 0x27);
		map(InputConstants.KEY_EQUALS, 0x0D);
		map(InputConstants.KEY_LBRACKET, 0x1A);
		map(InputConstants.KEY_BACKSLASH, 0x2B);
		map(InputConstants.KEY_RBRACKET, 0x1B);
		map(InputConstants.KEY_GRAVE, 0x29);
		map(InputConstants.KEY_ESCAPE, 0x01);
		map(InputConstants.KEY_RETURN, 0x1C);
		map(InputConstants.KEY_TAB, 0x0F);
		map(InputConstants.KEY_BACKSPACE, 0x0E);
		map(InputConstants.KEY_INSERT, 0xD2);
		map(InputConstants.KEY_DELETE, 0xD3);
		map(InputConstants.KEY_RIGHT, 0xCD);
		map(InputConstants.KEY_LEFT, 0xCB);
		map(InputConstants.KEY_DOWN, 0xD0);
		map(InputConstants.KEY_UP, 0xC8);
		map(InputConstants.KEY_PAGEUP, 0xC9);
		map(InputConstants.KEY_PAGEDOWN, 0xD1);
		map(InputConstants.KEY_HOME, 0xC7);
		map(InputConstants.KEY_END, 0xCF);
		map(InputConstants.KEY_CAPSLOCK, 0x3A);
		map(InputConstants.KEY_SCROLLLOCK, 0x46);
		map(InputConstants.KEY_NUMLOCK, 0x45);
		map(InputConstants.KEY_PRINTSCREEN, 0xB7);
		map(InputConstants.KEY_PAUSE, 0xC5);
		int[] numpad = {0x52, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49};
		for (int i = 0; i < 10; i++) {
			map(InputConstants.KEY_NUMPAD0 + i, numpad[i]);
		}
		map(InputConstants.KEY_NUMPADCOMMA, 0x53);
		map(331, 0xB5); // GLFW_KEY_KP_DIVIDE
		map(InputConstants.KEY_MULTIPLY, 0x37);
		map(333, 0x4A); // GLFW_KEY_KP_SUBTRACT
		map(InputConstants.KEY_ADD, 0x4E);
		map(InputConstants.KEY_NUMPADENTER, 0x9C);
		map(InputConstants.KEY_LSHIFT, 0x2A);
		map(InputConstants.KEY_LCONTROL, 0x1D);
		map(InputConstants.KEY_LALT, 0x38);
		map(343, 0xDB); // GLFW_KEY_LEFT_SUPER
		map(InputConstants.KEY_RSHIFT, 0x36);
		map(InputConstants.KEY_RCONTROL, 0x9D);
		map(InputConstants.KEY_RALT, 0xB8);
		map(347, 0xDC); // GLFW_KEY_RIGHT_SUPER
		map(348, 0xDD); // GLFW_KEY_MENU
	}

	private WcKeyCodes() {
	}

	private static void map(int glfw, int dik) {
		GLFW_TO_DIK[glfw] = dik;
		DIK_TO_GLFW[dik] = glfw;
	}

	/** The DIK for a GLFW key, or 0 if the Witcher side can't see that key. */
	public static int toDik(int glfw) {
		return glfw >= 0 && glfw < GLFW_TO_DIK.length ? GLFW_TO_DIK[glfw] : 0;
	}

	/** The GLFW key for a DIK, or -1. */
	public static int toGlfw(int dik) {
		return dik > 0 && dik < 256 && DIK_TO_GLFW[dik] != 0 ? DIK_TO_GLFW[dik] : -1;
	}

	/** What a host key (DIK) is called, in Minecraft's words. */
	public static Component name(int dik) {
		if (dik == WcOptions.KEY_NONE || dik <= 0) {
			return Component.literal("none");
		}
		int glfw = toGlfw(dik);
		return glfw < 0 ? Component.literal("key " + dik) : InputConstants.Type.KEYBOARD.getOrCreate(glfw).getDisplayName();
	}

	/** The key a Minecraft key binding is on, as a DIK (KEY_NONE unbound, KEY_MOUSE a mouse button). */
	public static int dikOf(KeyMapping mapping) {
		if (mapping == null || mapping.isUnbound()) {
			return WcOptions.KEY_NONE;
		}
		InputConstants.Key key = InputConstants.getKey(mapping.saveString());
		if (key.getType() != InputConstants.Type.KEYBOARD) {
			return WcOptions.KEY_MOUSE;
		}
		int dik = toDik(key.getValue());
		return dik == 0 ? WcOptions.KEY_NONE : dik;
	}

	/**
	 * Keys the Witcher side keeps for itself (never reach Minecraft, or would break the game):
	 * Esc (Minecraft's menu), ` (the Witcher's console), F9-F12, Alt, the Windows keys and Print Screen.
	 */
	public static boolean reserved(int dik) {
		return dik == 0x01 || dik == 0x29 || dik == 0x43 || dik == 0x44 || dik == 0x57 || dik == 0x58 || dik == 0x38 || dik == 0xB8 || dik == 0xDB
			|| dik == 0xDC || dik == 0xB7;
	}

	/** F1-F12 (the host's switch key works with a Minecraft screen open only for these). */
	public static boolean isFunctionKey(int dik) {
		return (dik >= 0x3B && dik <= 0x44) || dik == 0x57 || dik == 0x58;
	}

	/** M, J, I, K, N: the Witcher's map, journal, inventory, character and meditation keys. */
	public static boolean panelKey(int dik) {
		return dik == 0x32 || dik == 0x24 || dik == 0x17 || dik == 0x25 || dik == 0x31;
	}
}
