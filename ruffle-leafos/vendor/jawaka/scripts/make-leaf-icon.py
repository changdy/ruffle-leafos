#!/usr/bin/env python3
"""Generate the Leaf app/default icon for system_icons/.

Original work (MIT, top-level Jawaka LICENSE). 256x256 to match the libretro
Systematic console icons: a single flat leaf with a darker outline border and
simple veins, in the muted-flat style of the console art it sits beside.
"""
import math
import os

from PIL import Image, ImageDraw

SS = 4                      # supersample factor (render big, downscale smooth)
SIZE = 256
N = SIZE * SS
CX = CY = N / 2

# Soft "Leaf" greens, muted to match the flat console art next to them.
FILL   = (134, 183, 106, 255)
BORDER = (90, 134, 64, 255)
VEIN   = (108, 156, 80, 255)

LEN  = 220 * SS             # leaf length (tip to base); sized to fill the diagonal
HALF = LEN / 2
WIDE = 70 * SS             # half-width control magnitude
TILT = math.radians(-135)  # tip to lower-left, stem to upper-right


def bezier(p0, p1, p2, p3, steps):
    pts = []
    for i in range(steps + 1):
        t = i / steps
        mt = 1 - t
        x = mt**3 * p0[0] + 3 * mt * mt * t * p1[0] + 3 * mt * t * t * p2[0] + t**3 * p3[0]
        y = mt**3 * p0[1] + 3 * mt * mt * t * p1[1] + 3 * mt * t * t * p2[1] + t**3 * p3[1]
        pts.append((x, y))
    return pts


def place(p):
    """Rotate a leaf-local point by TILT and move it to the frame center."""
    x, y = p
    ca, sa = math.cos(TILT), math.sin(TILT)
    return (CX + x * ca - y * sa, CY + x * sa + y * ca)


img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
draw = ImageDraw.Draw(img)

tip  = (0, -HALF)
base = (0,  HALF)

# Two sides of the blade, base -> tip, bulging out.
right = bezier(base, (WIDE * 1.18, HALF * 0.45), (WIDE * 1.18, -HALF * 0.28), tip, 90)
left  = bezier(base, (-WIDE * 1.18, HALF * 0.45), (-WIDE * 1.18, -HALF * 0.28), tip, 90)
blade = right + left[::-1]
blade_pts = [place(p) for p in blade]

# Body + a clear border stroke (the "border like the game systems have").
draw.polygon(blade_pts, fill=FILL)
draw.line(blade_pts + [blade_pts[0]], fill=BORDER, width=5 * SS, joint="curve")

# Midrib.
midrib = bezier(base, (0, HALF * 0.3), (0, -HALF * 0.3), (0, -HALF * 0.95), 50)
draw.line([place(p) for p in midrib], fill=VEIN, width=4 * SS, joint="curve")

# A few side veins off the midrib, kept inside the blade.
for frac, reach in ((0.45, 0.72), (0.18, 0.66), (-0.10, 0.52)):
    y0 = HALF * frac
    rise = HALF * 0.16
    rv = bezier((0, y0), (WIDE * 0.35, y0 - rise * 0.4),
                (WIDE * reach * 0.8, y0 - rise * 0.8),
                (WIDE * reach, y0 - rise), 24)
    lv = bezier((0, y0), (-WIDE * 0.35, y0 - rise * 0.4),
                (-WIDE * reach * 0.8, y0 - rise * 0.8),
                (-WIDE * reach, y0 - rise), 24)
    draw.line([place(p) for p in rv], fill=VEIN, width=3 * SS, joint="curve")
    draw.line([place(p) for p in lv], fill=VEIN, width=3 * SS, joint="curve")

# Stem out of the base.
stem = bezier(base, (0, HALF * 1.03), (WIDE * 0.06, HALF * 1.14),
              (WIDE * 0.14, HALF * 1.2), 20)
draw.line([place(p) for p in stem], fill=BORDER, width=6 * SS, joint="curve")

img = img.resize((SIZE, SIZE), Image.LANCZOS)
out = os.path.join(os.path.dirname(__file__), "..", "res", "system_icons", "_apps.png")
img.save(os.path.normpath(out))
print("wrote", os.path.normpath(out))
