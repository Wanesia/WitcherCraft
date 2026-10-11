package dev.witchercraft.client;

import com.mojang.blaze3d.pipeline.RenderTarget;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.renderpearl.api.commands.RenderPass;
import com.mojang.renderpearl.api.commands.RenderPassDescriptor;
import com.mojang.renderpearl.api.textures.FilterMode;
import com.mojang.renderpearl.api.textures.GpuTexture;
import com.mojang.renderpearl.api.textures.GpuTextureView;
import dev.witchercraft.WitcherCraft;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.navigation.ScreenRectangle;
import net.minecraft.client.renderer.RenderPipelines;
import net.minecraft.client.renderer.state.gui.GuiRenderState;
import net.minecraft.client.renderer.state.level.CameraRenderState;
import org.joml.Matrix4f;

/**
 * WitcherCraft occlusion: Minecraft's depth for the frame the overlay ships,
 * so the host can hide Minecraft pixels that Witcher geometry is in front of.
 *
 * Per frame (all GPU-side copies, no CPU work on the render thread):
 *  1. {@link #saveLevelDepth}: right before the first-person hand pass clears the depth buffer, the
 *     level's depth is copied into our own texture.
 *  2. {@link #finishFrame}: after the hand was drawn, vanilla's INTEGRATE_DEPTH pass writes the
 *     hand's depth over it (where the hand is), so the hand is never hidden by the Witcher world.
 *  3. {@link #captureGuiMask}: just before the 2D GUI renders, the screen tiles its elements cover
 *     are noted; FrameExporter forces those to "no depth" (= always on top) when it ships the frame.
 * FrameExporter then reads our texture back asynchronously next to the colour.
 *
 * Off with -Dwitchercraft.depth=false (or without the world layer).
 */
public final class DepthExport {
	public static final boolean ENABLED = !"false".equals(System.getProperty("witchercraft.depth", "true"));
	public static final int TILE = 16;

	private static GpuTexture texture;
	private static GpuTextureView view;
	private static int texW, texH;
	private static boolean savedThisFrame;
	private static boolean ready;
	private static boolean failed;
	private static boolean loggedOnce;
	private static float a, b;
	// GUI tile mask of this frame (16 px tiles, top-down rows), filled before the GUI renders.
	private static long[] guiMask = new long[0];
	private static int maskTilesX, maskTilesY, maskCount;
	private static boolean screenOpen;

	private DepthExport() {
	}

	public static boolean active() {
		return ENABLED && WorldLayer.active() && !failed;
	}

	private static boolean ensureTexture(GpuTexture mainDepth, int w, int h) {
		if (texture != null && texW == w && texH == h && texture.getFormat() == mainDepth.getFormat()) {
			return true;
		}
		if (view != null) {
			view.close();
		}
		if (texture != null) {
			texture.close();
		}
		texture = RenderSystem.getDevice().createTexture(() -> "WitcherCraft depth export", 15, mainDepth.getFormat(), w, h, 1, 1);
		view = RenderSystem.getDevice().createTextureView(texture);
		texW = w;
		texH = h;
		WitcherCraft.LOG.info("WitcherCraft occlusion: depth export {}x{} {} (level + hand, reversed Z: {})", w, h, mainDepth.getFormat(),
			RenderSystem.getDevice().getDeviceInfo().isZZeroToOne() ? "zero-to-one" : "minus-one-to-one");
		return true;
	}

	/** Before render3dHud clears the depth buffer: copy the level's depth. */
	public static void saveLevelDepth(Minecraft minecraft) {
		if (!active()) {
			return;
		}
		try {
			RenderTarget target = minecraft.gameRenderer.mainRenderTarget();
			GpuTexture depth = target.getDepthTexture();
			if (depth == null) {
				return;
			}
			if (!ensureTexture(depth, target.width, target.height)) {
				return;
			}
			RenderSystem.getDevice().createCommandEncoder().copyTextureToTexture(depth, texture, 0, 0, 0, 0, 0, target.width, target.height);
			savedThisFrame = true;
		} catch (RuntimeException e) {
			fail("saving the level depth", e);
		}
	}

	/** After the level and the hand (before post effects and the GUI): merge the hand's depth. */
	public static void finishFrame(Minecraft minecraft) {
		ready = false;
		if (!active()) {
			savedThisFrame = false;
			return;
		}
		try {
			RenderTarget target = minecraft.gameRenderer.mainRenderTarget();
			GpuTexture depth = target.getDepthTexture();
			if (depth == null) {
				return;
			}
			if (!savedThisFrame) {
				// No hand pass this frame (hidden GUI, spectator...): the main depth is the level's.
				if (!ensureTexture(depth, target.width, target.height)) {
					return;
				}
				RenderSystem.getDevice().createCommandEncoder().copyTextureToTexture(depth, texture, 0, 0, 0, 0, 0, target.width, target.height);
			} else {
				// Vanilla's INTEGRATE_DEPTH: gl_FragDepth = hand depth wherever the hand wrote one (else discard).
				RenderPassDescriptor desc = RenderPassDescriptor.builder(() -> "WitcherCraft hand depth").withDepthAttachment(view).build();
				try (RenderPass pass = RenderSystem.getDevice().createCommandEncoder().createRenderPass(desc)) {
					pass.setUniform("InSampler", target.getDepthTextureView(), RenderSystem.getSamplerCache().getClampToEdge(FilterMode.NEAREST));
					pass.setPipeline(RenderSystem.getCompiledPipeline(RenderPipelines.INTEGRATE_DEPTH));
					pass.draw(3, 1, 0, 0);
				}
			}
			savedThisFrame = false;
			// z = b / (d - a) from the level projection (reversed Z: far/near swapped in setPerspective).
			CameraRenderState cam = minecraft.gameRenderer.gameRenderState().levelRenderState.cameraRenderState;
			Matrix4f p = cam.projectionMatrix;
			if (p != null) {
				// clip.z = m22 * zv + m32, clip.w = -zv (zv < 0 in front) => ndc = -m22 + m32 / z.
				boolean zeroToOne = RenderSystem.getDevice().getDeviceInfo().isZZeroToOne();
				float na = -p.m22(), nb = p.m32();
				a = zeroToOne ? na : 0.5F * na + 0.5F;
				b = zeroToOne ? nb : 0.5F * nb;
				if (!loggedOnce) {
					loggedOnce = true;
					WitcherCraft.LOG.info("WitcherCraft occlusion: depth model z = {} / (d - {}) (1 m -> d = {}, 100 m -> d = {})", b, a, a + b,
						a + b / 100.0F);
				}
			}
			ready = p != null;
		} catch (RuntimeException e) {
			fail("merging the hand depth", e);
		}
	}

	/** Just before the GUI renders: which 16 px tiles its elements cover (they stay on top). */
	public static void captureGuiMask(Minecraft minecraft, GuiRenderState gui) {
		screenOpen = minecraft.gui.screen() != null;
		maskCount = 0;
		if (!active() || !ready) {
			return;
		}
		try {
			int w = minecraft.gameRenderer.mainRenderTarget().width, h = minecraft.gameRenderer.mainRenderTarget().height;
			maskTilesX = (w + TILE - 1) / TILE;
			maskTilesY = (h + TILE - 1) / TILE;
			int words = (maskTilesX * maskTilesY + 63) / 64;
			if (guiMask.length < words) {
				guiMask = new long[words];
			} else {
				java.util.Arrays.fill(guiMask, 0, words, 0L);
			}
			int scale = Math.max(1, minecraft.getWindow().getGuiScale());
			// The crosshair and the attack indicator under it are NOT marked: their tiles hold the
			// block being aimed at, which then showed through Witcher walls as a square around the
			// crosshair. They are depth-tested like the level instead.
			int cx = w / scale / 2, cy = h / scale / 2;
			gui.forEachElement(e -> {
				ScreenRectangle r = e.bounds();
				if (r != null && r.left() >= cx - 10 && r.right() <= cx + 10 && r.top() >= cy - 10 && r.bottom() <= cy + 16) {
					return;
				}
				mark(r, scale);
			}, GuiRenderState.TraverseRange.ALL);
			gui.forEachText(t -> mark(t.bounds(), scale));
			gui.forEachItem(i -> mark(i.bounds(), scale));
			gui.forEachPictureInPicture(p -> mark(p.bounds(), scale));
		} catch (RuntimeException e) {
			fail("collecting the GUI mask", e);
		}
	}

	private static void mark(ScreenRectangle r, int scale) {
		if (r == null || r.width() <= 0 || r.height() <= 0) {
			return;
		}
		int x0 = Math.max(0, r.left() * scale / TILE), y0 = Math.max(0, r.top() * scale / TILE);
		int x1 = Math.min(maskTilesX - 1, (r.right() * scale - 1) / TILE), y1 = Math.min(maskTilesY - 1, (r.bottom() * scale - 1) / TILE);
		for (int y = y0; y <= y1; y++) {
			for (int x = x0; x <= x1; x++) {
				int i = y * maskTilesX + x;
				long bit = 1L << (i & 63);
				if ((guiMask[i >> 6] & bit) == 0) {
					guiMask[i >> 6] |= bit;
					maskCount++;
				}
			}
		}
	}

	/** The depth to read back for this frame, or null (no depth: the host draws without occlusion). */
	public static GpuTexture frameTexture(int w, int h) {
		boolean ok = active() && ready && !screenOpen && texture != null && texW == w && texH == h;
		ready = false; // one capture per finished frame
		return ok ? texture : null;
	}

	public static float modelA() {
		return a;
	}

	public static float modelB() {
		return b;
	}

	/** A copy of this frame's GUI mask: {tilesX, tilesY, count} + bits. */
	public static long[] snapshotMask(int[] dims) {
		dims[0] = maskTilesX;
		dims[1] = maskTilesY;
		dims[2] = maskCount;
		if (maskCount == 0) {
			return null;
		}
		return java.util.Arrays.copyOf(guiMask, (maskTilesX * maskTilesY + 63) / 64);
	}

	/** After a readback error: no more depth (the colour-only path is the proven one). */
	public static void disable() {
		failed = true;
	}

	private static void fail(String what, RuntimeException e) {
		if (!failed) {
			failed = true;
			WitcherCraft.LOG.error("WitcherCraft occlusion: depth export disabled ({} failed); the Witcher draws Minecraft without occlusion", what, e);
		}
	}
}
