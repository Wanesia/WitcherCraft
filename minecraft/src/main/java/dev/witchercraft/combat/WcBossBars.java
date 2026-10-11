package dev.witchercraft.combat;

import dev.witchercraft.WcOptions;
import dev.witchercraft.link.SkyLink;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import net.minecraft.network.chat.Component;
import net.minecraft.server.level.ServerBossEvent;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.BossEvent;

/**
 * WitcherCraft: Witcher bosses get Minecraft's boss bar (name + health at the top of the screen)
 * while they fight within 64 blocks, instead of the Witcher's own boss bar (option "Minecraft
 * boss bar"; the script hides BossFocusModule then).
 */
public final class WcBossBars {
	private static final int BOSS = 1 << 4, IN_COMBAT = 1 << 3, HOSTILE = 1;
	private static final double RANGE = 64.0;
	private static final Map<Integer, ServerBossEvent> BARS = new HashMap<>();

	private WcBossBars() {
	}

	/** Server thread, after each actor table read. */
	public static void update(List<SkyLink.Actor> actors, List<ServerPlayer> players) {
		Set<Integer> keep = new HashSet<>();
		if (WcOptions.mcBossBar) {
			for (SkyLink.Actor a : actors) {
				if ((a.flags() & BOSS) == 0 || a.dead() || (a.flags() & (IN_COMBAT | HOSTILE)) == 0) {
					continue;
				}
				ServerPlayer near = null;
				for (ServerPlayer p : players) {
					if (p.distanceToSqr(a.x(), a.y(), a.z()) < RANGE * RANGE) {
						near = p;
					}
				}
				if (near == null) {
					continue;
				}
				keep.add(a.formId());
				ServerBossEvent bar = BARS.get(a.formId());
				if (bar == null) {
					String name = a.name().isEmpty() ? "Boss" : a.name();
					bar = new ServerBossEvent(java.util.UUID.randomUUID(), Component.literal(name), BossEvent.BossBarColor.RED, BossEvent.BossBarOverlay.PROGRESS);
					BARS.put(a.formId(), bar);
					dev.witchercraft.WitcherCraft.LOG.info("WitcherCraft: boss bar for {}", name);
				}
				bar.setProgress(Math.max(0.0F, Math.min(1.0F, a.healthFrac())));
				for (ServerPlayer p : players) {
					if (p.distanceToSqr(a.x(), a.y(), a.z()) < RANGE * RANGE) {
						bar.addPlayer(p);
					} else {
						bar.removePlayer(p);
					}
				}
			}
		}
		BARS.entrySet().removeIf(e -> {
			if (keep.contains(e.getKey())) {
				return false;
			}
			e.getValue().removeAllPlayers();
			return true;
		});
	}

	public static void clear() {
		for (ServerBossEvent bar : BARS.values()) {
			bar.removeAllPlayers();
		}
		BARS.clear();
	}
}
