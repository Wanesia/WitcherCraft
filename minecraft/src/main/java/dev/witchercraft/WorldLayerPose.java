package dev.witchercraft;

import org.joml.Matrix4f;
import org.joml.Quaternionf;
import org.joml.Vector3f;

/**
 * The world-layer camera angles the Witcher camera copies (FrameExporter.renderedCameraPose):
 * Minecraft's own yaw and pitch, plus the small extra rotation of the walk bob / hurt tilt.
 *
 * The whole camera rotation is NOT turned into Euler angles: at pitch +-90 (looking straight down
 * to build under yourself, or straight up) yaw and roll become the same axis there, the
 * decomposition picks an arbitrary split that jumps from frame to frame, and the Witcher camera
 * spun. Only the bob, a rotation of a few degrees about the view, is decomposed; it never gets
 * near that singularity. Exact for the walk bob (a roll and a nod about the view axes, so
 * R * bob^-1 = Ry(yaw) Rx(pitch - nod) Rz(-roll)); for the hurt tilt it is a close approximation.
 */
public final class WorldLayerPose {
	private WorldLayerPose() {
	}

	/**
	 * {yaw, pitch, roll} in degrees, Minecraft's convention (Camera.setRotation: orientation =
	 * rotationYXZ(180 - yaw, -pitch, roll)), from the camera's yRot / xRot and the inverse of the
	 * bob matrix GameRenderer folds into the projection.
	 */
	public static double[] angles(float yRot, float xRot, Matrix4f bobInverse) {
		Vector3f b = bobInverse.getNormalizedRotation(new Quaternionf()).getEulerAnglesYXZ(new Vector3f());
		double yaw = wrapDegrees(yRot - Math.toDegrees(b.y));
		// The walk bob's nod can push a straight-down look a few degrees past 90; kept at 90 so the
		// Witcher camera never gets a pitch past vertical (the same view with yaw and roll flipped).
		double pitch = Math.max(-90.0, Math.min(90.0, xRot - Math.toDegrees(b.x)));
		double roll = Math.toDegrees(b.z);
		return new double[] { yaw, pitch, roll };
	}

	private static double wrapDegrees(double d) {
		d %= 360.0;
		if (d >= 180.0) {
			d -= 360.0;
		}
		if (d < -180.0) {
			d += 360.0;
		}
		return d;
	}
}
