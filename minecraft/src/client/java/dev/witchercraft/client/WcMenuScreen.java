package dev.witchercraft.client;

import dev.witchercraft.WitcherCraft;
import dev.witchercraft.WcOptions;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.client.gui.components.Button;
import net.minecraft.client.gui.components.Tooltip;
import net.minecraft.client.gui.layouts.LinearLayout;
import net.minecraft.client.gui.screens.ConfirmScreen;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.network.chat.Component;

/** Esc menu -> "WitcherCraft...": about the mod, its options, its keys, and reset / all off. */
public final class WcMenuScreen extends WcScreen {
	public WcMenuScreen(Screen parent) {
		super(Component.literal("WitcherCraft"), parent);
	}

	static String version() {
		return FabricLoader.getInstance().getModContainer(WitcherCraft.MOD_ID).map(c -> c.getMetadata().getVersion().getFriendlyString()).orElse("?");
	}

	@Override
	protected void addContents(LinearLayout column) {
		column.addChild(this.text("Minecraft inside The Witcher 3. You are Steve in Geralt's world; "
			+ WcKeyCodes.name(WcOptions.modeKey).getString() + " switches to Geralt and back. Mod version " + version() + "."));
		column.addChild(this.button("About WitcherCraft...", "What the mod is, how it works, what works and what doesn't yet.",
			() -> this.minecraft.gui.setScreen(new WcAboutScreen(this))));
		column.addChild(this.button("Options...", "Turn the mod's features on or off: the Witcher's HUD, fire, climbing help, torch light, speed...",
			() -> this.minecraft.gui.setScreen(new WcOptionsScreen(this))));
		column.addChild(this.button("Controls...", "Change or unbind the mod's keys: switch to Geralt, interact, Witcher senses, call Roach.",
			() -> this.minecraft.gui.setScreen(new WcControlsScreen(this))));
		column.addChild(this.button("Reset everything to defaults", "All WitcherCraft options and keys back to how they were installed.", () -> this.confirm(
			"Reset WitcherCraft to defaults?", "Every WitcherCraft option and key goes back to how it was installed. Minecraft's own settings stay as they are.",
			() -> {
				WcOptions.resetDefaults();
				WcKeys.resetToDefaults(this.minecraft);
				DestructionToggle.resetDefault();
				WitcherCraft.LOG.info("WitcherCraft: options and keys reset to defaults");
			})));
		column.addChild(this.button("Turn all extras off", "Plain Minecraft in the Witcher: every optional WitcherCraft feature off.", () -> this.confirm(
			"Turn all WitcherCraft extras off?",
			"Off: the Witcher's minimap, quest tracker and enemy names, boss bars, key hints, climbing help, torch light, "
				+ "the M/J/I/K/N panel keys, Witcher senses and Call Roach keys. Steve gets normal Minecraft speed and hunger.\n"
				+ "Kept: walking, building and fighting in the Witcher's world, the interact key and " + WcKeyCodes.name(WcOptions.modeKey).getString()
				+ " to switch to Geralt. Each can be turned back on in Options and Controls.",
			() -> {
				WcOptions.allExtrasOff();
				WcKeys.unbindExtras(this.minecraft);
				WitcherCraft.LOG.info("WitcherCraft: all extras off");
			})));
	}

	private Button button(String label, String tip, Runnable action) {
		return Button.builder(Component.literal(label), b -> action.run()).width(220).tooltip(Tooltip.create(Component.literal(tip))).build();
	}

	private void confirm(String title, String message, Runnable onYes) {
		this.minecraft.gui.setScreen(new ConfirmScreen(yes -> {
			if (yes) {
				onYes.run();
			}
			this.minecraft.gui.setScreen(this);
		}, Component.literal(title), Component.literal(message)));
	}
}
