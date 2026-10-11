package dev.witchercraft.client;

import com.mojang.blaze3d.pipeline.RenderTarget;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.renderpearl.api.buffers.GpuBuffer;
import com.mojang.renderpearl.api.buffers.GpuBufferSlice;
import com.mojang.renderpearl.api.textures.GpuTexture;
import dev.witchercraft.WitcherCraft;
import dev.witchercraft.link.Proto;
import dev.witchercraft.link.SkyLink;
import com.mojang.blaze3d.vertex.PoseStack;
import dev.witchercraft.client.mixin.GameRendererInvoker;
import java.lang.foreign.MemorySegment;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.state.level.CameraRenderState;
import net.minecraft.world.phys.Vec3;
import org.joml.Matrix4f;
import org.joml.Quaternionf;
import org.joml.Vector3f;
import org.lwjgl.opengl.GL11C;
import org.lwjgl.opengl.GL32C;

/**
 * Copies Minecraft's main render target (hand + HUD + screens on a transparent background) back
 * from the GPU and publishes it to Skyrim through the overlay triple buffer.
 *
 * WitcherCraft world layer: the same target then also holds the level (blocks, entities,
 * particles, outline), still on a transparent background (see {@link WorldLayer}). The frame is
 * flagged OVERLAY_WORLD_LAYER and carries the camera it was rendered with.
 *
 * The copy is asynchronous: a frame is captured into one of a few staging buffers and shipped
 * once the GPU says the copy finished, typically a frame later.
 *
 * WitcherCraft perf: the readback is flushed to the GPU right away and tracked with
 * our own GL fence. Minecraft's fenced callbacks only run once per frame (before rendering) and its
 * fence covers the whole frame's submit, which nothing flushes early with the window hidden: frames
 * then completed in pairs and the shipper (newest only) dropped ~20% of them in game (captured 115
 * fps, shipped 91), so The Witcher's camera repeated a pose every 4th-5th frame (judder).
 */
public final class FrameExporter {
	private static final int STAGING = 3;
	private static final int FREE = 0;
	private static final int PENDING = 1;
	private static final int READY = 2;

	private static final Staging[] staging = new Staging[STAGING];
	private static long nextFrameId = 1;
	private static boolean loggedFormat;
	private static boolean checkedEuler;

	// World-layer performance numbers, logged every 10 s.
	private static long statSince;
	private static int statCaptured, statShipped, statSkipped;
	private static long statShipNs, statLatencyNs, statIntervalNs, statIntervalMax, lastCaptureNs;
	// Perf: frames dropped (a newer one was ready too), and the render thread's waits for the previous
	// frame's readback.
	private static int statDropped, statWaited;
	private static long statWaitNs;
	/** Longest wait (ns) at capture for the previous frame's readback (else it ships a frame late). */
	private static final long WAIT_PREVIOUS_NS = Long.getLong("witchercraft.shipWaitUs", 3000L) * 1000L;
	/** -Dwitchercraft.shipLegacy=true or env WC_SHIP_LEGACY: the old callback-only shipping (A/B). */
	private static final boolean LEGACY_SHIP = Boolean.getBoolean("witchercraft.shipLegacy") || System.getenv("WC_SHIP_LEGACY") != null;

	private static final class Staging {
		GpuBuffer buffer;
		int width;
		int height;
		volatile int state = FREE;
		long frameId;
		int flags;
		double[] pose;
		long capturedNs;
		// WitcherCraft occlusion: Minecraft's depth of the same frame (DepthExport), read back alongside.
		GpuBuffer depthBuffer;
		long depthBytes;
		boolean hasDepth;
		volatile boolean colorDone, depthDone;
		float depthA, depthB;
		long[] guiMask;
		int maskTilesX, maskTilesY, maskCount;
		long glSync; // our own fence behind the readback copies (0 = none: Minecraft's callback decides)
	}

	private static int statDepth;
	private static boolean loggedDepthSkip;

	private FrameExporter() {
	}

	public static void capture(Minecraft minecraft) {
		if (!LEGACY_SHIP) {
			pollFences(WAIT_PREVIOUS_NS);
		}
		shipReadyFrames();

		RenderTarget target = minecraft.gameRenderer.mainRenderTarget();
		GpuTexture color = target.getColorTexture();
		if (color == null) {
			return;
		}
		int width = target.width;
		int height = target.height;
		if (width > Proto.MAX_OVERLAY_W || height > Proto.MAX_OVERLAY_H) {
			return;
		}
		if (!loggedFormat) {
			loggedFormat = true;
			WitcherCraft.LOG.info("WitcherCraft: overlay capture {}x{} format {}{}", width, height, color.getFormat(),
				WorldLayer.ENABLED ? " (WitcherCraft world layer: full frame, transparent background)" : "");
		}
		long now = System.nanoTime();
		if (lastCaptureNs != 0) {
			long interval = now - lastCaptureNs;
			statIntervalNs += interval;
			statIntervalMax = Math.max(statIntervalMax, interval);
		}
		lastCaptureNs = now;

		Staging slot = null;
		for (Staging s : staging) {
			if (s != null && s.state == FREE) {
				slot = s;
				break;
			}
		}
		if (slot == null) {
			for (int i = 0; i < STAGING; i++) {
				if (staging[i] == null) {
					staging[i] = slot = new Staging();
					break;
				}
			}
		}
		if (slot == null) {
			statSkipped++;
			return; // all staging buffers still in flight; skip this frame
		}

		long bytes = (long) width * height * 4L;
		if (slot.buffer == null || slot.width != width || slot.height != height) {
			if (slot.buffer != null) {
				slot.buffer.close();
			}
			slot.buffer = RenderSystem.getDevice().createBuffer(() -> "WitcherCraft overlay readback", 9, bytes);
			slot.width = width;
			slot.height = height;
		}
		final Staging captured = slot;
		deleteSync(captured);
		captured.state = PENDING;
		captured.colorDone = false;
		captured.depthDone = true;
		captured.hasDepth = false;
		GpuTexture depth = WorldLayer.active() ? DepthExport.frameTexture(width, height) : null;
		if (depth != null && !Proto.overlayDepthFits(width, height)) {
			if (!loggedDepthSkip) {
				loggedDepthSkip = true;
				WitcherCraft.LOG.warn("WitcherCraft occlusion: {}x{} colour + depth do not fit an overlay slot; no depth (no occlusion)", width, height);
			}
			depth = null;
		}
		if (depth != null) {
			long dBytes = (long) width * height * 4L;
			if (captured.depthBuffer == null || captured.depthBytes != dBytes) {
				if (captured.depthBuffer != null) {
					captured.depthBuffer.close();
				}
				captured.depthBuffer = RenderSystem.getDevice().createBuffer(() -> "WitcherCraft depth readback", 9, dBytes);
				captured.depthBytes = dBytes;
			}
			captured.hasDepth = true;
			captured.depthDone = false;
			captured.depthA = DepthExport.modelA();
			captured.depthB = DepthExport.modelB();
			int[] dims = new int[3];
			captured.guiMask = DepthExport.snapshotMask(dims);
			captured.maskTilesX = dims[0];
			captured.maskTilesY = dims[1];
			captured.maskCount = dims[2];
		}
		captured.frameId = nextFrameId++;
		captured.flags = Proto.OVERLAY_BOTTOM_UP;
		captured.pose = null;
		if (WorldLayer.active()) {
			captured.flags |= Proto.OVERLAY_WORLD_LAYER;
			captured.pose = renderedCameraPose(minecraft);
		}
		captured.capturedNs = now;
		statCaptured++;
		try {
			captureCopies(captured, color, depth, width, height);
		} catch (RuntimeException e) {
			// Never take Minecraft down for the overlay: drop this frame, and the depth export for good.
			captured.state = FREE;
			statSkipped++;
			if (!loggedCopyError) {
				loggedCopyError = true;
				WitcherCraft.LOG.error("WitcherCraft: overlay readback failed; depth export disabled, frame skipped", e);
			}
			DepthExport.disable();
		}
	}

	private static boolean loggedCopyError;

	private static void captureCopies(Staging captured, GpuTexture color, GpuTexture depth, int width, int height) {
		// ONE fence per frame, as before the depth export: the depth readback is issued first and the GPU
		// runs both copies in order, so the colour copy's fence also covers the depth. (Two fences made
		// frames complete in pairs, and the shipper, which only ships the newest, dropped ~40% of them.)
		if (captured.hasDepth) {
			readDepth(depth, captured.depthBuffer, width, height);
		}
		final long id = captured.frameId;
		RenderSystem.getDevice().createCommandEncoder().copyTextureToBuffer(color, captured.buffer, 0L, () -> {
			// Minecraft's own (later) fence: only if ours didn't already mark this frame and the slot
			// wasn't reused for a newer capture since.
			if (captured.frameId == id && captured.state == PENDING) {
				markReady(captured);
			}
		}, 0);
		if (!LEGACY_SHIP && color instanceof com.mojang.renderpearl.backend.opengl.GlTexture) {
			// Our fence right behind the copies, and a flush so the GPU starts them now.
			captured.glSync = GL32C.glFenceSync(GL32C.GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
			GL11C.glFlush();
		}
	}

	private static void deleteSync(Staging s) {
		if (s.glSync != 0) {
			GL32C.glDeleteSync(s.glSync);
			s.glSync = 0;
		}
	}

	private static void markReady(Staging s) {
		s.colorDone = true;
		s.depthDone = true;
		s.state = READY;
		deleteSync(s);
	}

	private static boolean signaled(int r) {
		return r == GL32C.GL_ALREADY_SIGNALED || r == GL32C.GL_CONDITION_SATISFIED;
	}

	/**
	 * Marks finished readbacks READY by our own fences. If the newest pending one (normally the
	 * previous frame) isn't finished, waits for it up to {@code waitNs}: shipped a frame late it would
	 * lose to the next one (dropped), and The Witcher would show a frame with no new Minecraft pose.
	 */
	private static void pollFences(long waitNs) {
		Staging newestPending = null;
		for (Staging s : staging) {
			if (s == null || s.state != PENDING || s.glSync == 0) {
				continue;
			}
			int r = GL32C.glClientWaitSync(s.glSync, 0, 0L);
			if (signaled(r)) {
				markReady(s);
			} else if (r == GL32C.GL_WAIT_FAILED) {
				deleteSync(s); // Minecraft's callback takes over for this one
			} else if (newestPending == null || s.frameId > newestPending.frameId) {
				newestPending = s;
			}
		}
		if (newestPending == null || waitNs <= 0) {
			return;
		}
		long t0 = System.nanoTime();
		int r = GL32C.glClientWaitSync(newestPending.glSync, GL32C.GL_SYNC_FLUSH_COMMANDS_BIT, waitNs);
		statWaited++;
		statWaitNs += System.nanoTime() - t0;
		if (signaled(r)) {
			// Everything older is done too (the GPU runs the copies in order).
			for (Staging s : staging) {
				if (s != null && s.state == PENDING && s.glSync != 0 && s.frameId <= newestPending.frameId) {
					markReady(s);
				}
			}
		}
	}

	/**
	 * Ships a finished frame without waiting. Any time on the render thread (e.g. while Minecraft
	 * waits for the next Witcher frame in SkyClient.paceFrame), so The Witcher gets it earlier.
	 */
	public static void pollShip() {
		if (LEGACY_SHIP) {
			return;
		}
		pollFences(0L);
		shipReadyFrames();
	}

	/**
	 * The camera the level was just rendered with, including Minecraft's hurt tilt and walk bob
	 * (which GameRenderer.renderLevel() folds into the projection matrix): {eyeX, eyeY, eyeZ, yaw,
	 * pitch, fovDeg, rollDeg}, MC coordinates/degrees. Orientation = rotationYXZ(180 - yaw, -pitch, roll),
	 * i.e. exactly Camera.setRotation's convention plus a roll about the view axis.
	 */
	private static double[] renderedCameraPose(Minecraft minecraft) {
		var state = minecraft.gameRenderer.gameRenderState();
		CameraRenderState cam = state.levelRenderState.cameraRenderState;
		if (!cam.initialized) {
			return null; // first frames after joining: no camera yet (no OVERLAY_HAS_POSE)
		}
		PoseStack bob = new PoseStack();
		GameRendererInvoker renderer = (GameRendererInvoker) minecraft.gameRenderer;
		renderer.witchercraft$bobHurt(cam, bob);
		if (state.optionsRenderState.bobView) {
			renderer.witchercraft$bobView(cam, bob);
		}
		// clip = P * bob * R^-1 * (p - pos)  =>  camera-to-world = R * bob^-1
		Matrix4f camToWorld = new Matrix4f().rotation(cam.orientation).mul(new Matrix4f(bob.last().pose()).invert());
		Vector3f offset = camToWorld.transformPosition(new Vector3f());
		Quaternionf rotation = camToWorld.getNormalizedRotation(new Quaternionf());
		// Not the Euler angles of the whole rotation: they spin at pitch +-90 (WorldLayerPose).
		double[] angles = dev.witchercraft.WorldLayerPose.angles(cam.yRot, cam.xRot, new Matrix4f(bob.last().pose()).invert());
		double yaw = angles[0];
		double pitch = angles[1];
		double roll = angles[2];
		if (!checkedEuler) {
			// One-time self-check: the angles rebuild the rendered rotation (WorldLayerPoseTest too).
			checkedEuler = true;
			Quaternionf back = new Quaternionf().rotationYXZ((float) Math.toRadians(180.0 - yaw), (float) Math.toRadians(-pitch), (float) Math.toRadians(roll));
			double err = Math.toDegrees(back.difference(rotation, new Quaternionf()).angle());
			WitcherCraft.LOG.info("WitcherCraft world layer: camera pose self-check (angle error {} deg, roll {} deg)",
				String.format("%.4f", Math.min(err, 360.0 - err)), String.format("%.3f", roll)); // q and -q: the same rotation
		}
		Vec3 pos = cam.pos;
		return new double[] { pos.x + offset.x, pos.y + offset.y, pos.z + offset.z, yaw, pitch,
			minecraft.gameRenderer.mainCamera().getFov(), roll };
	}

	/** Maps the newest finished readback and copies it into shared memory. */
	private static void shipReadyFrames() {
		// Ships the NEWEST finished frame (and frees the older ones). Shipping every frame in a burst
		// was tried: the host then overwrote ready-but-not-yet-uploaded frames
		// whose camera the Witcher had already used, breaking the exact frame match. Keep this.
		Staging newest = null;
		for (Staging s : staging) {
			if (s != null && s.state == READY && (newest == null || s.frameId > newest.frameId)) {
				newest = s;
			}
		}
		if (newest == null) {
			return;
		}
		ship(newest);
		for (Staging s : staging) {
			if (s != null && s.state == READY && s.frameId <= newest.frameId) {
				if (s != newest) {
					statDropped++;
				}
				s.state = FREE;
			}
		}
		logStats(newest.width, newest.height);
	}

	private static void ship(Staging st) {
		MemorySegment shm = SkyLink.segment();
		if (shm == null) {
			return;
		}
		long t0 = System.nanoTime();
		long bytes = (long) st.width * st.height * 4L;
		try (GpuBufferSlice.MappedView view = st.buffer.map(true, false)) {
			MemorySegment src = MemorySegment.ofBuffer(view.data());
			MemorySegment.copy(src, 0, shm, SkyLink.overlayBackSlotOffset(), Math.min(bytes, src.byteSize()));
		}
		int flags = st.flags;
		if (st.hasDepth && shipDepth(shm, st)) {
			flags |= Proto.OVERLAY_HAS_DEPTH;
			statDepth++;
		}
		SkyLink.publishOverlay(st.width, st.height, flags, st.frameId, st.pose);
		long t1 = System.nanoTime();
		statShipped++;
		statShipNs += t1 - t0;
		statLatencyNs += t1 - st.capturedNs;
	}

	private static int depthFbo;

	/**
	 * Async readback of a depth texture. On the OpenGL backend NOT through
	 * CommandEncoder.copyTextureToBuffer: for depth formats that calls glReadBuffer(GL_NONE) on the
	 * encoder's shared read framebuffer and never restores it, so the next COLOUR readback fails with
	 * GL error 1282 ("Couldn't perform copyToBuffer for texture Main / Color"). We read through our
	 * own framebuffer instead (GlStateManager keeps its binding cache right) and leave the shared one alone.
	 */
	private static void readDepth(GpuTexture depth, GpuBuffer buffer, int width, int height) {
		if (depth instanceof com.mojang.renderpearl.backend.opengl.GlTexture glTex
			&& buffer instanceof com.mojang.renderpearl.backend.opengl.GlBuffer glBuf) {
			if (depthFbo == 0) {
				depthFbo = com.mojang.renderpearl.backend.opengl.GlStateManager.glGenFramebuffers();
			}
			com.mojang.renderpearl.backend.opengl.GlStateManager._glBindFramebuffer(0x8CA8, depthFbo); // GL_READ_FRAMEBUFFER
			com.mojang.renderpearl.backend.opengl.GlStateManager._glFramebufferTexture2D(0x8CA8, 0x8D00, 0x0DE1, glTex.glId(), 0); // DEPTH_ATTACHMENT, TEXTURE_2D
			com.mojang.renderpearl.backend.opengl.GlStateManager._glReadBuffer(0); // GL_NONE: only on our own framebuffer
			com.mojang.renderpearl.backend.opengl.GlStateManager._glBindBuffer(0x88EB, glBuf.handle()); // GL_PIXEL_PACK_BUFFER
			com.mojang.renderpearl.backend.opengl.GlStateManager._pixelStore(0x0D05, 4); // GL_PACK_ALIGNMENT
			com.mojang.renderpearl.backend.opengl.GlStateManager._pixelStore(0x0D02, 0); // GL_PACK_ROW_LENGTH
			com.mojang.renderpearl.backend.opengl.GlStateManager._readPixels(0, 0, width, height, 0x1902, 0x1406, 0L); // DEPTH_COMPONENT, FLOAT
			com.mojang.renderpearl.backend.opengl.GlStateManager._glBindFramebuffer(0x8CA8, 0);
			com.mojang.renderpearl.backend.opengl.GlStateManager._glBindBuffer(0x88EB, 0);
			return;
		}
		RenderSystem.getDevice().createCommandEncoder().copyTextureToBuffer(depth, buffer, 0L, () -> {}, 0);
	}

	/** Depth plane + header after the colour in the back slot; HUD/screen tiles forced to "no depth". */
	private static boolean shipDepth(MemorySegment shm, Staging st) {
		long hdr = SkyLink.overlayBackSlotOffset() + Proto.overlayDepthOffset(st.width, st.height);
		long data = hdr + Proto.OVERLAY_DEPTH_DATA_OFF;
		long bytes = (long) st.width * st.height * 4L;
		try (GpuBufferSlice.MappedView view = st.depthBuffer.map(true, false)) {
			MemorySegment src = MemorySegment.ofBuffer(view.data());
			if (src.byteSize() < bytes) {
				return false;
			}
			MemorySegment.copy(src, 0, shm, data, bytes);
		}
		// GUI tiles: rows in the plane are bottom-up (GL), the mask is top-down.
		if (st.guiMask != null) {
			int tile = DepthExport.TILE;
			for (int ty = 0; ty < st.maskTilesY; ty++) {
				for (int tx = 0; tx < st.maskTilesX; tx++) {
					int i = ty * st.maskTilesX + tx;
					if ((st.guiMask[i >> 6] & (1L << (i & 63))) == 0) {
						continue;
					}
					int x0 = tx * tile, x1 = Math.min(st.width, x0 + tile);
					for (int y = ty * tile; y < Math.min(st.height, ty * tile + tile); y++) {
						long row = data + ((long) (st.height - 1 - y) * st.width + x0) * 4L;
						shm.asSlice(row, (long) (x1 - x0) * 4L).fill((byte) 0); // 0.0f = clearDepth (reversed Z)
					}
				}
			}
		}
		shm.set(java.lang.foreign.ValueLayout.JAVA_INT, hdr + Proto.DH_MAGIC, Proto.OVERLAY_DEPTH_MAGIC);
		shm.set(java.lang.foreign.ValueLayout.JAVA_INT, hdr + Proto.DH_WIDTH, st.width);
		shm.set(java.lang.foreign.ValueLayout.JAVA_INT, hdr + Proto.DH_HEIGHT, st.height);
		shm.set(java.lang.foreign.ValueLayout.JAVA_INT, hdr + Proto.DH_FORMAT, 1);
		shm.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, hdr + Proto.DH_A, st.depthA);
		shm.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, hdr + Proto.DH_B, st.depthB);
		shm.set(java.lang.foreign.ValueLayout.JAVA_FLOAT, hdr + Proto.DH_CLEAR, 0.0F);
		shm.set(java.lang.foreign.ValueLayout.JAVA_INT, hdr + Proto.DH_MASKED_TILES, st.maskCount);
		shm.set(java.lang.foreign.ValueLayout.JAVA_LONG, hdr + Proto.DH_FRAME_ID, st.frameId);
		return true;
	}

	private static void logStats(int width, int height) {
		if (!WorldLayer.ENABLED) {
			return;
		}
		long now = System.nanoTime();
		if (statSince == 0) {
			statSince = now;
			return;
		}
		if (now - statSince < 10_000_000_000L || statShipped == 0) {
			return;
		}
		double secs = (now - statSince) / 1e9;
		WitcherCraft.LOG.info(
			"WitcherCraft world layer: {}x{} captured {} fps, shipped {} fps, skipped {}, dropped {}; frame interval avg {} ms max {} ms; "
				+ "map+copy {} ms/frame; capture->shipped {} ms; waited for the GPU {} times, avg {} ms; with depth {}{}",
			width, height, String.format("%.1f", statCaptured / secs), String.format("%.1f", statShipped / secs), statSkipped, statDropped,
			String.format("%.2f", statCaptured > 1 ? statIntervalNs / 1e6 / (statCaptured - 1) : 0.0),
			String.format("%.2f", statIntervalMax / 1e6),
			String.format("%.2f", statShipNs / 1e6 / statShipped), String.format("%.2f", statLatencyNs / 1e6 / statShipped), statWaited,
			String.format("%.2f", statWaited > 0 ? statWaitNs / 1e6 / statWaited : 0.0), statDepth, LEGACY_SHIP ? " (legacy shipping)" : "");
		statSince = now;
		statCaptured = statShipped = statSkipped = statDepth = statDropped = statWaited = 0;
		statShipNs = statLatencyNs = statIntervalNs = statIntervalMax = statWaitNs = 0;
	}
}
