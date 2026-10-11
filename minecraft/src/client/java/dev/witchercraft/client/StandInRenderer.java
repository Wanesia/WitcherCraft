package dev.witchercraft.client;

import dev.witchercraft.WcOptions;
import net.minecraft.client.renderer.entity.EntityRendererProvider;
import net.minecraft.client.renderer.entity.NoopRenderer;
import net.minecraft.world.entity.Entity;

/**
 * The Witcher draws the real character; its Minecraft stand-in is only a hitbox. With the
 * "Minecraft name tags" option, its Witcher name (the stand-in's custom name, SkyCombat) floats
 * over every Witcher character within NAME_RANGE blocks, like a named Minecraft mob's.
 */
public final class StandInRenderer<T extends Entity> extends NoopRenderer<T> {
	private static final double NAME_RANGE = 16.0;
	private static boolean loggedFirstTag;

	public StandInRenderer(EntityRendererProvider.Context context) {
		super(context);
	}

	@Override
	protected boolean shouldShowName(T entity, double distanceToCameraSq) {
		boolean show = WcOptions.mcNameTags && entity.hasCustomName() && distanceToCameraSq < NAME_RANGE * NAME_RANGE;
		if (show && !loggedFirstTag) {
			loggedFirstTag = true;
			dev.witchercraft.WitcherCraft.LOG.info("WitcherCraft: first Minecraft name tag shown: {}", entity.getCustomName().getString());
		}
		return show;
	}
}
