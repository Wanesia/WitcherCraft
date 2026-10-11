package dev.witchercraft;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.util.Random;
import org.joml.Matrix4f;
import org.joml.Quaternionf;
import org.joml.Vector3f;
import org.junit.jupiter.api.Test;

/**
 * The world-layer camera pose (FrameExporter.renderedCameraPose) relies on JOML's
 * getEulerAnglesYXZ being the inverse of rotationYXZ, and on the camera-to-world composition
 * R * bob^-1 putting a pure view-space roll into the Z angle.
 */
class WorldLayerPoseTest {
	@Test
	void eulerYXZRoundTrips() {
		Random random = new Random(42);
		for (int i = 0; i < 1000; i++) {
			float y = (random.nextFloat() * 2 - 1) * (float) Math.PI;
			float x = (random.nextFloat() * 2 - 1) * 1.5F; // pitch within +-86 degrees
			float z = (random.nextFloat() * 2 - 1) * 0.5F;
			Quaternionf q = new Quaternionf().rotationYXZ(y, x, z);
			Vector3f e = q.getEulerAnglesYXZ(new Vector3f());
			Quaternionf back = new Quaternionf().rotationYXZ(e.y, e.x, e.z);
			// Compare what the rotations do (quaternion angle() is too imprecise in float near 0).
			for (Vector3f v : new Vector3f[] { new Vector3f(0, 0, -1), new Vector3f(0, 1, 0) }) {
				Vector3f a = q.transform(new Vector3f(v)), b = back.transform(new Vector3f(v));
				assertEquals(0.0, a.distance(b), 1e-4);
			}
		}
	}

	@Test
	void viewSpaceRollBecomesZAngle() {
		// MC camera: yaw 30, pitch 20 -> rotationYXZ(pi - yaw, -pitch, 0). A bob that rolls the
		// view by +3 degrees (poseStack.rotateDegrees(Axis.ZP, 3)) is a camera roll of -3.
		float yaw = 30, pitch = 20;
		Quaternionf r = new Quaternionf().rotationYXZ((float) Math.PI - (float) Math.toRadians(yaw), (float) -Math.toRadians(pitch), 0);
		Matrix4f bob = new Matrix4f().rotateZ((float) Math.toRadians(3));
		Matrix4f camToWorld = new Matrix4f().rotation(r).mul(new Matrix4f(bob).invert());
		Vector3f e = camToWorld.getNormalizedRotation(new Quaternionf()).getEulerAnglesYXZ(new Vector3f());
		assertEquals(yaw, 180.0 - Math.toDegrees(e.y), 1e-3);
		assertEquals(pitch, -Math.toDegrees(e.x), 1e-3);
		assertEquals(-3.0, Math.toDegrees(e.z), 1e-3);
	}
	// ---- WorldLayerPose: the angles the Witcher camera copies -------------------------------------

	/** The camera-to-world rotation Minecraft renders with: R(yaw, pitch) * bob^-1. */
	private static Quaternionf rendered(float yaw, float pitch, Matrix4f bob) {
		Quaternionf r = new Quaternionf().rotationYXZ((float) Math.PI - (float) Math.toRadians(yaw), (float) -Math.toRadians(pitch), 0);
		return new Matrix4f().rotation(r).mul(new Matrix4f(bob).invert()).getNormalizedRotation(new Quaternionf());
	}

	private static double rebuildError(double[] a, Quaternionf rendered) {
		Quaternionf back = new Quaternionf().rotationYXZ((float) Math.toRadians(180.0 - a[0]), (float) Math.toRadians(-a[1]), (float) Math.toRadians(a[2]));
		// Compare what the rotations do to the view axes (quaternion angle() is too imprecise in float).
		double worst = 0;
		for (Vector3f v : new Vector3f[] { new Vector3f(0, 0, -1), new Vector3f(0, 1, 0) }) {
			worst = Math.max(worst, back.transform(new Vector3f(v)).distance(rendered.transform(new Vector3f(v))));
		}
		return Math.toDegrees(worst); // chord length ~ angle in radians for small angles
	}

	/** A walk bob as GameRenderer.bobView builds it: translate, roll about Z, nod about X. */
	private static Matrix4f walkBob(float rollDeg, float nodDeg) {
		return new Matrix4f().translate(0.02F, -0.03F, 0).rotateZ((float) Math.toRadians(rollDeg)).rotateX((float) Math.toRadians(nodDeg));
	}

	@Test
	void straightDownAndUpKeepMinecraftsYaw() {
		// Building under yourself: pitch exactly 90. The yaw must stay Minecraft's, not jump.
		for (float yaw : new float[] { -179.5F, -90F, -12.3F, 0F, 45F, 135.7F, 180F }) {
			for (float pitch : new float[] { 90F, -90F }) {
				double[] a = dev.witchercraft.WorldLayerPose.angles(yaw, pitch, new Matrix4f().invert());
				assertEquals(0.0, Math.abs(((a[0] - yaw) % 360 + 540) % 360 - 180), 1e-4, "yaw at pitch " + pitch);
				assertEquals(pitch, a[1], 1e-4);
				assertEquals(0.0, a[2], 1e-4);
			}
		}
	}

	@Test
	void anglesRebuildTheRenderedRotationEverywhere() {
		// Every pitch from straight up to straight down, with and without a walk bob.
		Random random = new Random(7);
		for (int i = 0; i < 2000; i++) {
			float yaw = (random.nextFloat() * 2 - 1) * 180F;
			float pitch = i < 200 ? (i % 2 == 0 ? 90F : -90F) : (random.nextFloat() * 2 - 1) * 90F;
			Matrix4f bob = i % 3 == 0 ? new Matrix4f() : walkBob((random.nextFloat() * 2 - 1) * 3F, random.nextFloat() * 5F);
			double[] a = dev.witchercraft.WorldLayerPose.angles(yaw, pitch, new Matrix4f(bob).invert());
			assertEquals(true, Math.abs(a[1]) <= 90.0, "pitch past vertical: " + a[1]);
			if (Math.abs(a[1]) < 90.0) { // at the cap the nod past vertical is dropped on purpose
				assertEquals(0.0, rebuildError(a, rendered(yaw, pitch, bob)), 0.01, "yaw " + yaw + " pitch " + pitch);
			}
		}
	}

	@Test
	void noJumpThroughStraightDown() {
		// Looking down past 89.99 degrees and back with a walk bob: the yaw moves smoothly.
		Matrix4f bob = walkBob(1.5F, 2.0F);
		double last = Double.NaN;
		for (int i = 0; i <= 400; i++) {
			float pitch = 88F + 2F * Math.min(i, 400 - i) / 200F; // 88 -> 90 -> 88
			double[] a = dev.witchercraft.WorldLayerPose.angles(37F, pitch, new Matrix4f(bob).invert());
			if (!Double.isNaN(last)) {
				assertEquals(last, a[0], 0.5, "yaw jumped at pitch " + pitch);
			}
			last = a[0];
		}
	}
}
