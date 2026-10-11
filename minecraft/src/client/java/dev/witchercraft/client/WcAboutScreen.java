package dev.witchercraft.client;

import dev.witchercraft.WcOptions;
import net.minecraft.client.KeyMapping;
import net.minecraft.client.gui.layouts.LinearLayout;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.network.chat.Component;

/** Esc menu -> WitcherCraft... -> About: what the mod is, how it works, what is and isn't ported. */
public final class WcAboutScreen extends WcScreen {
	public WcAboutScreen(Screen parent) {
		super(Component.literal("About WitcherCraft"), parent);
	}

	private static String key(int dik) {
		return WcKeyCodes.name(dik).getString();
	}

	private static String key(KeyMapping mapping) {
		return mapping == null || mapping.isUnbound() ? "(unbound)" : mapping.getTranslatedKeyMessage().getString();
	}

	@Override
	protected void addContents(LinearLayout column) {
		String mode = key(WcOptions.modeKey);
		String interact = WcOptions.interactKey == WcOptions.KEY_NONE ? "(unbound)" : key(WcOptions.interactKey);

		column.addChild(this.text("WitcherCraft " + WcMenuScreen.version() + ": Minecraft inside The Witcher 3. You play Steve in Geralt's world: "
			+ "walk its roads, build in it, dig into it and fight its monsters with Minecraft weapons."));

		column.addChild(this.heading("How it works"), s -> s.paddingTop(6));
		column.addChild(this.text("Two games run side by side and talk through shared memory. The Witcher 3 runs a small native plugin and scripts. "
			+ "Minecraft (this window, hidden) runs this mod, which is built on SkyCraft, the Skyrim version by chasmlol.\n"
			+ "- The Witcher sends its world: the ground, walls and buildings (from its own physics), water, and the characters near you.\n"
			+ "- Minecraft moves Steve with its own physics, renders its blocks, items and hand, and the Witcher draws that picture over its own, "
			+ "with Witcher walls hiding the blocks behind them.\n"
			+ "- Geralt walks along hidden under Steve, so the Witcher's world, characters and quests follow you."));

		column.addChild(this.heading("Playing"), s -> s.paddingTop(6));
		column.addChild(this.text("- Minecraft mode (the default): you are Steve. M map, J journal, I inventory and K character open the Witcher's "
			+ "panels while you stay Steve (Minecraft waits meanwhile); N: Geralt meditates, then Steve is back.\n"
			+ "- " + mode + " switches to Witcher mode, where you are Geralt again: his signs, swords and saving. " + mode
			+ " again brings Steve back where Geralt stands.\n"
			+ "- " + interact + ": talk, loot, open doors, mount Roach. Hold " + key(WcKeys.senses) + ": Witcher senses. " + key(WcKeys.roach)
			+ ": call Steve's Roach.\n"
			+ "- Esc: this menu. Save from Geralt's menu (" + mode + ", then Esc). Minecraft saves its own world by itself.\n"
			+ "- All keys: WitcherCraft... > Controls. Features on and off: WitcherCraft... > Options."));

		column.addChild(this.heading("What works"), s -> s.paddingTop(6));
		column.addChild(this.text("- Movement: walking, sprinting, jumping, elytra flight, swimming in the Witcher's lakes and seas, climbing help onto ledges, "
			+ "ender pearls across the Witcher's land.\n"
			+ "- Solid world: the Witcher's ground, walls, buildings, crates and fences block Steve.\n"
			+ "- Building and mining: Minecraft blocks stand in the Witcher's world; with World destruction on, mining and TNT dig into its ground. "
			+ "Steve has one world for the whole game: a build in Velen also stands at the same spot in Skellige and Toussaint.\n"
			+ "- Combat: Minecraft swords, axes, bows, tridents and TNT hurt Witcher enemies (a diamond sword kills a bandit in about 3 hits) "
			+ "and they fight back: their hits land on Steve, Minecraft armour and shields protect you.\n"
			+ "- Minecraft mobs join the fight: iron golems and tamed wolves attack Witcher enemies and leave you alone; zombies, skeletons, "
			+ "creepers and other hostile mobs attack them too, but go for whoever is closer.\n"
			+ "- Fire: lava, fire, flint and steel (use it on an enemy), fire charges, Fire Aspect and Flame arrows set enemies burning the Witcher's way. "
			+ "Water (pour a bucket on them, or a splash water bottle) puts them out.\n"
			+ "- The Witcher's minimap and quest tracker while you are Steve, Minecraft boss bars for Witcher bosses.\n"
			+ "- Talking, looting, doors and Roach with the interact key; Witcher senses; Steve's own Roach (a Minecraft horse).\n"
			+ "- Torches, lanterns, lava and other glowing blocks light the Witcher's world.\n"
			+ "- Dying as Steve: you respawn on the spot with your things; Geralt is unharmed."));

		column.addChild(this.heading("If something goes wrong"), s -> s.paddingTop(6));
		column.addChild(this.text("- " + mode + " gives you Geralt back at any time.\n"
			+ "- In the WitcherCraft... menu, \"Turn all extras off\" leaves only the basics and \"Reset everything to defaults\" undoes any change.\n"
			+ "- Logs: The Witcher 3\\bin\\x64_dx12\\witchercraft_native.log and Minecraft's logs\\latest.log. "
			+ "They show your Windows user name and Minecraft name, so blank those out before you post them anywhere."));
	}
}
