# M6b: measures the occlusion "edge fringe" in an overlay_test occl capture.
#
#   python occl_fringe.py <capture_on.bmp> [out_crop.png]
#
# Pillar A (the W3 occluder, flat orange) stands in front of Minecraft's wall, table and pig. Every
# pixel inside it must be exactly pillar colour: any deviation is Minecraft leaking through. The
# interior mask is the pillar's orange pixels, closed by 3 px (so thin leak lines inside count as
# interior) and then shrunk by 4 px (the pillar's own silhouette is legitimately soft: PCF).
# Prints the number of interior pixels, the max deviation (0..255) and how many deviate by > 4.
import sys

import numpy as np
from PIL import Image

PILLAR = np.array([230, 102, 26], dtype=np.int32)  # srgb (0.90, 0.40, 0.10)


def shift_max(m, r):
    out = m.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            out |= np.roll(np.roll(m, dy, 0), dx, 1)
    return out


def shift_min(m, r):
    out = m.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            out &= np.roll(np.roll(m, dy, 0), dx, 1)
    return out


img = np.asarray(Image.open(sys.argv[1]).convert('RGB')).astype(np.int32)
dev = np.abs(img - PILLAR).max(axis=2)
orange = dev <= 6
interior = shift_min(shift_max(orange, 3), 3 + 4)
# keep only the big orange component region (the pillar): rows/cols bounding box of the mask
d = dev[interior]
n = int(interior.sum())
print(f'interior pixels {n}, max deviation {int(d.max()) if n else -1}, pixels deviating > 4: {int((d > 4).sum())}, '
      f'mean deviation {float(d.mean()) if n else -1:.3f}')
if len(sys.argv) > 2 and n:
    ys, xs = np.nonzero(interior & (dev > 4))
    if len(ys):
        cy, cx = int(np.median(ys)), int(np.median(xs))
    else:
        ys, xs = np.nonzero(interior)
        cy, cx = int(np.median(ys)), int(np.median(xs))
    if len(sys.argv) > 4:  # explicit crop centre (same crop for before/after)
        cx, cy = int(sys.argv[3]), int(sys.argv[4])
    h, w = img.shape[:2]
    x0, y0 = max(0, min(w - 160, cx - 80)), max(0, min(h - 120, cy - 60))
    crop = Image.fromarray(img[y0:y0 + 120, x0:x0 + 160].astype(np.uint8)).resize((640, 480), Image.NEAREST)
    crop.save(sys.argv[2])
    print(f'crop centre {cx} {cy} -> {sys.argv[2]}')
