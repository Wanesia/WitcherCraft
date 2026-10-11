package dev.witchercraft.client;

import dev.witchercraft.WcOptions;
import dev.witchercraft.link.SkyLink;
import java.util.ArrayList;
import java.util.List;
import net.minecraft.client.Minecraft;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.world.level.chunk.LevelChunkSection;

/**
 * WitcherCraft: Minecraft's light-emitting blocks near the player (torches, lanterns, lava,
 * glowstone, ...) for the Witcher side, which lights its own world around them. Once a second, the
 * loaded chunks within 3 of the player (the mirror world is mostly empty: sections without an
 * emitter are skipped by their palette), nearest 64 first.
 */
public final class WcLights {
	private static final int CHUNK_RADIUS = 3, MAX = 64;
	private static int ticks;
	private static long lastSignature = Long.MIN_VALUE;
	private static int version;

	private WcLights() {
	}

	/** End of each client tick. */
	public static void tick(Minecraft minecraft) {
		if (++ticks % 20 != 0 || minecraft.level == null || minecraft.player == null || !SkyLink.active()) {
			return;
		}
		List<int[]> found = new ArrayList<>();
		if (WcOptions.blockLights) {
			int pcx = minecraft.player.chunkPosition().x(), pcz = minecraft.player.chunkPosition().z();
			for (int cx = pcx - CHUNK_RADIUS; cx <= pcx + CHUNK_RADIUS; cx++) {
				for (int cz = pcz - CHUNK_RADIUS; cz <= pcz + CHUNK_RADIUS; cz++) {
					LevelChunk chunk = minecraft.level.getChunkSource().getChunk(cx, cz, false);
					if (chunk == null) {
						continue;
					}
					LevelChunkSection[] sections = chunk.getSections();
					for (int i = 0; i < sections.length; i++) {
						LevelChunkSection s = sections[i];
						if (s == null || s.hasOnlyAir() || !s.maybeHas(st -> st.getLightEmission() > 0)) {
							continue;
						}
						int by = chunk.getSectionYFromSectionIndex(i) * 16;
						for (int y = 0; y < 16; y++) {
							for (int z = 0; z < 16; z++) {
								for (int x = 0; x < 16; x++) {
									BlockState st = s.getBlockState(x, y, z);
									int level = st.getLightEmission();
									if (level > 0) {
										found.add(new int[] {cx * 16 + x, by + y, cz * 16 + z, level, kind(st)});
									}
								}
							}
						}
					}
				}
			}
			double px = minecraft.player.getX(), py = minecraft.player.getY(), pz = minecraft.player.getZ();
			found.sort((a, b) -> Double.compare(dist2(a, px, py, pz), dist2(b, px, py, pz)));
			if (found.size() > MAX) {
				found = new ArrayList<>(found.subList(0, MAX));
			}
		}
		long sig = found.size();
		for (int[] l : found) {
			sig = sig * 31 + l[0];
			sig = sig * 31 + l[1];
			sig = sig * 31 + l[2];
			sig = sig * 31 + l[3] * 8 + l[4];
		}
		if (sig == lastSignature) {
			return;
		}
		lastSignature = sig;
		SkyLink.writeLights(found, ++version);
	}

	private static double dist2(int[] l, double x, double y, double z) {
		double dx = l[0] + 0.5 - x, dy = l[1] + 0.5 - y, dz = l[2] + 0.5 - z;
		return dx * dx + dy * dy + dz * dz;
	}

	private static int kind(BlockState st) {
		String id = BuiltInRegistries.BLOCK.getKey(st.getBlock()).getPath();
		if (id.contains("soul")) {
			return 1;
		}
		if (id.contains("lava") || id.contains("magma")) {
			return 2;
		}
		if (id.contains("redstone_torch") || id.contains("redstone_wall_torch") || id.contains("redstone_ore")) {
			return 4;
		}
		if (id.contains("glowstone") || id.contains("sea_lantern") || id.contains("froglight") || id.contains("shroomlight") || id.contains("end_rod")
			|| id.contains("beacon") || id.contains("redstone_lamp") || id.contains("glow")) {
			return 3;
		}
		return 0;
	}
}
