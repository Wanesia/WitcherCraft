package dev.witchercraft.client;

import com.mojang.blaze3d.platform.InputConstants;
import dev.witchercraft.WitcherCraft;
import dev.witchercraft.WcOptions;
import java.util.ArrayList;
import java.util.List;
import net.minecraft.ChatFormatting;
import net.minecraft.client.KeyMapping;
import net.minecraft.client.gui.components.Button;
import net.minecraft.client.gui.components.StringWidget;
import net.minecraft.client.gui.components.Tooltip;
import net.minecraft.client.gui.layouts.GridLayout;
import net.minecraft.client.gui.layouts.LinearLayout;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.client.gui.screens.options.controls.KeyBindsScreen;
import net.minecraft.client.input.KeyEvent;
import net.minecraft.network.chat.Component;
import net.minecraft.network.chat.MutableComponent;
import org.jspecify.annotations.Nullable;

/**
 * WitcherCraft's keys (Esc menu -> WitcherCraft... -> Controls). Click a key, then press the new
 * one; Esc unbinds it (the Steve / Geralt switch can't be unbound: it is the only way back).
 *
 * <p>Two kinds: the interact key and the Steve / Geralt switch are read by the Witcher side's
 * input hook (stored as DirectInput codes in WcOptions, sent in McState wcKeys), so Minecraft never
 * sees them in Minecraft mode. Witcher senses and Call Roach are ordinary Minecraft key bindings.
 */
public final class WcControlsScreen extends WcScreen {
	private enum Slot {
		MODE("Switch Steve / Geralt", "Leave Minecraft mode to play Geralt (map, inventory, saving), and come back. Can't be unbound."),
		INTERACT("Interact", "Talk, loot, open doors, pick up, mount Roach: the Witcher's interaction while you are Steve."),
		SENSES("Witcher senses (hold)", "Hold for Geralt's Witcher senses while you are Steve (clues glow red, items yellow)."),
		ROACH("Call Roach", "Whistle for Steve's Roach, a tame Minecraft horse that follows you in the Witcher's world.");

		final String label, tip;

		Slot(String label, String tip) {
			this.label = label;
			this.tip = tip;
		}
	}

	private @Nullable Slot listening;
	private Component status = Component.empty();

	public WcControlsScreen(Screen parent) {
		super(Component.literal("WitcherCraft controls"), parent);
	}

	@Override
	protected void addContents(LinearLayout column) {
		column.addChild(this.text("Click a key, then press the new one. Esc while choosing: unbind (or cancel, for the switch)."));
		GridLayout grid = new GridLayout().columnSpacing(6).rowSpacing(4);
		GridLayout.RowHelper rows = grid.createRowHelper(3);
		for (Slot slot : Slot.values()) {
			rows.addChild(new StringWidget(120, 20, Component.literal(slot.label), this.font));
			List<String> problems = new ArrayList<>(), shared = new ArrayList<>();
			this.warnings(slot, problems, shared);
			MutableComponent keyText = this.listening == slot ? Component.literal("> press a key <").withStyle(ChatFormatting.YELLOW)
				: this.keyName(slot).copy();
			if (this.listening != slot && !problems.isEmpty()) {
				keyText = keyText.withStyle(ChatFormatting.RED);
			} else if (this.listening != slot && !shared.isEmpty()) {
				keyText = keyText.withStyle(ChatFormatting.YELLOW);
			}
			List<String> notes = new ArrayList<>(problems);
			notes.addAll(shared);
			String tip = slot.tip + (notes.isEmpty() ? "" : "\n\n" + String.join("\n", notes));
			rows.addChild(Button.builder(keyText, b -> {
				this.listening = slot;
				this.status = Component.empty();
				this.rebuildWidgets();
			}).width(110).tooltip(Tooltip.create(Component.literal(tip))).build());
			Button reset = Button.builder(Component.literal("Reset"), b -> {
				this.reset(slot);
				this.rebuildWidgets();
			}).width(50).tooltip(Tooltip.create(Component.literal("Back to " + this.defaultName(slot).getString()))).build();
			reset.active = !this.isDefault(slot);
			rows.addChild(reset);
		}
		column.addChild(grid, s -> s.paddingTop(4));
		column.addChild(new StringWidget(this.columnWidth(), 12, this.status, this.font));

		column.addChild(this.heading("Fixed keys"), s -> s.paddingTop(6));
		column.addChild(this.text("Esc: Minecraft's menu (this one). E: inventory. F5: camera. As Steve, M map, J journal, I inventory and K character "
			+ "open the Witcher's panels and you stay Steve; N: Geralt meditates, then Steve is back (Options: \"Panel keys (M/J/I/K/N)\"). "
			+ "` opens the Witcher's console. "
			+ "Minecraft's own keys are in Minecraft's Key Binds."));
		column.addChild(Button.builder(Component.literal("Minecraft key binds..."),
			b -> this.minecraft.gui.setScreen(new KeyBindsScreen(this, this.minecraft.options))).width(200).build());
	}

	// ---- key capture ----

	@Override
	public boolean keyPressed(KeyEvent event) {
		Slot slot = this.listening;
		if (slot == null) {
			return super.keyPressed(event);
		}
		this.listening = null;
		if (event.key() == InputConstants.KEY_ESCAPE) {
			if (slot == Slot.MODE) {
				this.status = Component.literal("The switch key can't be unbound: it is the only way back to Minecraft.").withStyle(ChatFormatting.YELLOW);
			} else {
				this.unbind(slot);
				this.status = Component.literal(slot.label + ": unbound.");
			}
		} else {
			String error = this.bind(slot, event.key());
			this.status = error == null ? Component.literal(slot.label + ": " + this.keyName(slot).getString() + ".")
				: Component.literal(error).withStyle(ChatFormatting.RED);
		}
		this.rebuildWidgets();
		return true;
	}

	@Override
	public boolean shouldCloseOnEsc() {
		return this.listening == null;
	}

	/** Returns null if bound, else why not. */
	private @Nullable String bind(Slot slot, int glfw) {
		int dik = WcKeyCodes.toDik(glfw);
		String name = InputConstants.Type.KEYBOARD.getOrCreate(glfw).getDisplayName().getString();
		if (dik == 0) {
			return name + " can't be used here (the Witcher side doesn't see it).";
		}
		if (WcKeyCodes.reserved(dik)) {
			return name + " belongs to the Witcher or Windows (menu, console, F9-F12, Alt, Windows keys, Print Screen). Pick another.";
		}
		if ((slot == Slot.MODE || slot == Slot.INTERACT) && WcOptions.panelKeys && WcKeyCodes.panelKey(dik)) {
			return name + " is a Witcher panel key. Pick another, or turn off \"Panel keys (M/J/I/K/N)\" in Options.";
		}
		if (slot == Slot.MODE && dik == WcOptions.interactKey || slot == Slot.INTERACT && dik == WcOptions.modeKey) {
			return name + " is already " + (slot == Slot.MODE ? "Interact" : "the Steve / Geralt switch") + ".";
		}
		switch (slot) {
			case MODE -> WcOptions.modeKey = dik;
			case INTERACT -> WcOptions.interactKey = dik;
			case SENSES, ROACH -> this.setMapping(slot, InputConstants.Type.KEYBOARD.getOrCreate(glfw));
		}
		WcOptions.save();
		WitcherCraft.LOG.info("WitcherCraft: {} -> {}", slot.label, name);
		return null;
	}

	private void unbind(Slot slot) {
		switch (slot) {
			case INTERACT -> {
				WcOptions.interactKey = WcOptions.KEY_NONE;
				WcOptions.save();
			}
			case SENSES, ROACH -> this.setMapping(slot, InputConstants.UNKNOWN);
			default -> {
			}
		}
		WitcherCraft.LOG.info("WitcherCraft: {} unbound", slot.label);
	}

	private void reset(Slot slot) {
		switch (slot) {
			case MODE -> WcOptions.modeKey = WcOptions.DIK_F8;
			case INTERACT -> WcOptions.interactKey = WcOptions.DIK_G;
			case SENSES, ROACH -> this.setMapping(slot, this.mapping(slot).getDefaultKey());
		}
		WcOptions.save();
		this.status = Component.literal(slot.label + ": back to " + this.keyName(slot).getString() + ".");
	}

	private void setMapping(Slot slot, InputConstants.Key key) {
		this.mapping(slot).setKey(key);
		KeyMapping.resetMapping();
		this.minecraft.options.save();
	}

	// ---- what's bound ----

	private KeyMapping mapping(Slot slot) {
		return slot == Slot.SENSES ? WcKeys.senses : WcKeys.roach;
	}

	/** The slot's key as a DIK (KEY_NONE unbound, KEY_MOUSE a mouse button). */
	private int dik(Slot slot) {
		return switch (slot) {
			case MODE -> WcOptions.modeKey;
			case INTERACT -> WcOptions.interactKey;
			case SENSES, ROACH -> WcKeyCodes.dikOf(this.mapping(slot));
		};
	}

	private Component keyName(Slot slot) {
		return switch (slot) {
			case MODE, INTERACT -> WcKeyCodes.name(this.dik(slot));
			case SENSES, ROACH -> this.mapping(slot).isUnbound() ? Component.literal("none") : this.mapping(slot).getTranslatedKeyMessage();
		};
	}

	private Component defaultName(Slot slot) {
		return switch (slot) {
			case MODE -> WcKeyCodes.name(WcOptions.DIK_F8);
			case INTERACT -> WcKeyCodes.name(WcOptions.DIK_G);
			case SENSES, ROACH -> this.mapping(slot).getDefaultKey().getDisplayName();
		};
	}

	private boolean isDefault(Slot slot) {
		return switch (slot) {
			case MODE -> WcOptions.modeKey == WcOptions.DIK_F8;
			case INTERACT -> WcOptions.interactKey == WcOptions.DIK_G;
			case SENSES, ROACH -> this.mapping(slot).isDefault();
		};
	}

	/**
	 * Why this slot's key may not do what you expect, explained in the tooltip. {@code problems}
	 * (red): it clashes with another WitcherCraft key or never reaches Minecraft. {@code shared}
	 * (yellow): a Minecraft action uses the same key. F3 debug combos don't count.
	 */
	private void warnings(Slot slot, List<String> problems, List<String> shared) {
		int dik = this.dik(slot);
		if (dik == WcOptions.KEY_NONE || dik == WcOptions.KEY_MOUSE) {
			return;
		}
		boolean host = slot == Slot.MODE || slot == Slot.INTERACT;
		for (Slot other : Slot.values()) {
			if (other != slot && this.dik(other) == dik) {
				problems.add("Also " + other.label + ".");
			}
		}
		if (!host && (dik == WcOptions.modeKey || dik == WcOptions.interactKey)) {
			problems.add("The Witcher side takes this key first, so Minecraft never gets it.");
		}
		if (!host && WcOptions.panelKeys && WcKeyCodes.panelKey(dik)) {
			problems.add("A Witcher panel key: it opens the Witcher's panel instead (see Options).");
		}
		int glfw = WcKeyCodes.toGlfw(dik);
		if (glfw >= 0) {
			InputConstants.Key key = InputConstants.Type.KEYBOARD.getOrCreate(glfw);
			for (KeyMapping km : this.minecraft.options.keyMappings) {
				if (km != WcKeys.senses && km != WcKeys.roach && !km.getName().startsWith("key.debug.") && km.matches(key)) {
					shared.add("Also Minecraft's \"" + Component.translatable(km.getName()).getString() + "\""
						+ (host ? ": as Steve this key does the WitcherCraft action instead." : ": both happen."));
				}
			}
		}
		if (slot == Slot.MODE && !WcKeyCodes.isFunctionKey(dik)) {
			shared.add("Not an F key: while you type (chat, signs) it types instead of switching.");
		}
	}
}
