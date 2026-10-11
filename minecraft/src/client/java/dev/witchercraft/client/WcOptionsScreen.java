package dev.witchercraft.client;

import dev.witchercraft.WcOptions;
import java.util.function.Consumer;
import net.minecraft.client.gui.components.CycleButton;
import net.minecraft.client.gui.components.Tooltip;
import net.minecraft.client.gui.layouts.GridLayout;
import net.minecraft.client.gui.layouts.LinearLayout;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.network.chat.Component;

/**
 * WitcherCraft options (Esc menu -> WitcherCraft... -> Options): what you see and how Steve plays.
 * Every change is saved at once (config/witchercraft.properties) and reaches the Witcher side
 * through McState. The keys are on the Controls screen.
 */
public final class WcOptionsScreen extends WcScreen {
	private GridLayout.RowHelper rows;
	private int cell;

	public WcOptionsScreen(Screen parent) {
		super(Component.literal("WitcherCraft options"), parent);
	}

	@Override
	protected void addContents(LinearLayout column) {
		this.cell = Math.min(200, (this.columnWidth() - 8) / 2);

		this.section(column, "The Witcher's screen while you play Steve");
		this.toggle("Witcher minimap", WcOptions.minimap, v -> WcOptions.minimap = v, "The Witcher's minimap while you play as Steve.");
		this.toggle("Quest tracker", WcOptions.questTracker, v -> WcOptions.questTracker = v, "The Witcher's active quest objectives.");
		this.toggle("Geralt's HUD", WcOptions.geraltHud, v -> WcOptions.geraltHud = v,
			"Geralt's health, medallion, buffs and quick slots (Minecraft shows Steve's own).");
		this.toggle("Witcher enemy names", WcOptions.witcherEnemyInfo, v -> WcOptions.witcherEnemyInfo = v,
			"The Witcher's name and health bar over the enemy you fight.");
		this.toggle("Minecraft name tags", WcOptions.mcNameTags, v -> WcOptions.mcNameTags = v,
			"Minecraft name tags over Witcher characters within 16 blocks.");
		this.toggle("Minecraft boss bar", WcOptions.mcBossBar, v -> WcOptions.mcBossBar = v,
			"Witcher bosses get Minecraft's boss bar (name and health at the top) instead of the Witcher's.");
		this.toggle("Key hints on mode switch", WcOptions.keyHints, v -> WcOptions.keyHints = v,
			"The short list of keys shown when you switch between Steve and Geralt.");

		this.section(column, "Steve");
		this.rows.addChild(CycleButton.<Integer>builder(v -> Component.literal(v + "%"), WcOptions.speedPct).withValues(boxed(WcOptions.SPEEDS))
			.withTooltip(v -> Tooltip.create(Component.literal("Steve's walking and sprinting speed. 100% is normal Minecraft; Geralt runs faster.")))
			.create(0, 0, this.cell, 20, Component.literal("Steve speed"), (b, v) -> {
				WcOptions.speedPct = v;
				WcOptions.save();
			}));
		this.toggle("Minecraft hunger", WcOptions.hunger, v -> WcOptions.hunger = v,
			"Off: Steve never gets too hungry to sprint (food can still heal you).");
		this.toggle("Climbing help", WcOptions.climbHelp, v -> WcOptions.climbHelp = v,
			"Jump at a ledge up to ~3 blocks high while walking at it to get on top.");

		this.section(column, "World and keys");
		this.toggle("Torch light in the Witcher", WcOptions.blockLights, v -> WcOptions.blockLights = v,
			"Torches, lanterns, glowstone, lava and other glowing blocks light up the Witcher's world around them.");
		this.toggle("Panel keys (M/J/I/K/N)", WcOptions.panelKeys, v -> WcOptions.panelKeys = v,
			"M map, J journal, I inventory, K character: as Steve, these open the Witcher's panel and you stay Steve "
				+ "(Minecraft waits meanwhile, even mid-flight). N: Geralt meditates, then Steve is back. "
				+ "Off: they are ordinary Minecraft keys.");
		this.toggle("Tips for turned-off keys", WcOptions.offKeyTips, v -> WcOptions.offKeyTips = v,
			"A short note (top right) when you press a key whose WitcherCraft feature is turned off, "
				+ "for example M while Panel keys is off.");
	}

	private void section(LinearLayout column, String title) {
		column.addChild(this.heading(title), s -> s.paddingTop(6));
		GridLayout grid = new GridLayout().columnSpacing(8).rowSpacing(4);
		this.rows = grid.createRowHelper(2);
		column.addChild(grid);
	}

	private void toggle(String label, boolean value, Consumer<Boolean> set, String tip) {
		this.rows.addChild(CycleButton.onOffBuilder(value).withTooltip(v -> Tooltip.create(Component.literal(tip)))
			.create(0, 0, this.cell, 20, Component.literal(label), (b, v) -> {
				set.accept(v);
				WcOptions.save();
				dev.witchercraft.WitcherCraft.LOG.info("WitcherCraft: option \"{}\" {}", label, v ? "on" : "off");
			}));
	}

	private static Integer[] boxed(int[] values) {
		Integer[] out = new Integer[values.length];
		for (int i = 0; i < values.length; i++) {
			out[i] = values[i];
		}
		return out;
	}
}
