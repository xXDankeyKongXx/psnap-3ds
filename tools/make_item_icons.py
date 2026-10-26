# Builds the bottom screen's item buttons (gfx/icon_*.png) from the game's own HUD icons, read from a
# course RAM snapshot (snapshots_full.txt; e.g. tools/tmp/course_ram.bin). The icons are 28x28 RGBA32
# sprites in app_level's data (odd rows stored with their 8-byte halves swapped, as loaded into TMEM).
# Each is a ring around the item with the N64 button's badge over its top-left: the badge is replaced
# by the ring's mirror image from the top right, gaps inside the circle are filled from the ring around
# them, everything outside the circle is cut away (soft edge), and the circle is scaled to 60x60.
# Usage: python -I tools/make_item_icons.py <ram snapshot>
import math
import sys

from PIL import Image

ROOT = __file__.replace("\\", "/").rsplit("/tools/", 1)[0]
ram = open(sys.argv[1], "rb").read()
ICONS = {"apple": 0x803867F8, "pester": 0x80385B58, "flute": 0x80387498, "dash": 0x80384218}
CX, CY, R = 14.5, 14.5, 12.2      # the ring (pixel centres at x + 0.5)
BX, BY, BR = 5.0, 5.0, 7.4        # the button badge
RING_INNER = 7.5                  # the ring band (the item inside it is left alone)
SIZE = 60


def byte(a):
    return ram[(a & 0xFFFFFF) ^ 3]


for name, base in ICONS.items():
    px = {}
    for y in range(28):
        for x in range(28):
            off = (y * 28 + x) * 4
            if y & 1:
                off ^= 8
            px[x, y] = [byte(base + off + c) for c in range(4)]
    # Badge -> the ring itself: each pixel of the ring band under the badge is the average of the ring
    # at the same distance from the centre, 90 degrees either side (bilinear samples), so the band's
    # radial shading carries on through it. The item inside the ring is left as it is.
    def sample(fx, fy):
        x0, y0 = int(math.floor(fx - 0.5)), int(math.floor(fy - 0.5))
        tx, ty = fx - 0.5 - x0, fy - 0.5 - y0
        acc = [0.0] * 4
        for (xx, yy, w) in ((x0, y0, (1 - tx) * (1 - ty)), (x0 + 1, y0, tx * (1 - ty)), (x0, y0 + 1, (1 - tx) * ty), (x0 + 1, y0 + 1, tx * ty)):
            q = px.get((xx, yy), [0, 0, 0, 0])
            for c in range(4):
                acc[c] += q[c] * w
        return acc

    def lum(q):
        return 0.299 * q[0] + 0.587 * q[1] + 0.114 * q[2]

    # Brightness of the ring next to the badge (top right and bottom left) and opposite it (bottom
    # right), for the flute's reflected fill.
    def ring_lum(ang):
        vals = [lum(sample(CX + d * math.cos(ang), CY + d * math.sin(ang))) for d in (9.0, 10.0, 11.0)]
        return sum(vals) / len(vals)
    near = (ring_lum(-math.pi / 4) + ring_lum(3 * math.pi / 4)) / 2
    far = ring_lum(math.pi / 4)
    gain = near / far if far > 1 else 1.0

    clean = {}
    for y in range(28):
        for x in range(28):
            p = px[x, y]
            dx, dy = x + 0.5 - CX, y + 0.5 - CY
            d = math.hypot(dx, dy)
            # The badge's quarter of the ring band (top left, centred on -135 degrees), faded over
            # 15 degrees at each end so the repair runs into the ring's own shading.
            ad = abs((math.degrees(math.atan2(dy, dx)) + 135 + 180) % 360 - 180)
            w = max(0.0, min(1.0, (60.0 - ad) / 15.0))
            if w > 0 and d > RING_INNER:
                if name == "flute":
                    # The flute crosses the ring from bottom left to top right: the ring's other
                    # side (reflected across that axis) is clean.
                    q = sample(CX - dy, CY - dx)
                    fill = [min(255, int(q[c] * gain)) for c in range(3)] + [255]
                else:
                    ang = math.atan2(dy, dx)
                    a = sample(CX + d * math.cos(ang + math.pi / 2), CY + d * math.sin(ang + math.pi / 2))
                    b = sample(CX + d * math.cos(ang - math.pi / 2), CY + d * math.sin(ang - math.pi / 2))
                    fill = [int((a[c] + b[c]) / 2) for c in range(3)] + [255]
                p = [int(fill[c] * w + p[c] * (1 - w)) for c in range(3)] + [int(fill[3] * w + p[3] * (1 - w))]
            clean[x, y] = p
    # Gaps: inside the circle every pixel is made opaque, its colour taken from the nearest opaque
    # pixel at the same distance from the centre (the ring's shading runs around it).
    out = Image.new("RGBA", (28, 28))
    for y in range(28):
        for x in range(28):
            p = clean[x, y]
            dx, dy = x + 0.5 - CX, y + 0.5 - CY
            d = math.hypot(dx, dy)
            if d < R and p[3] < 230:
                ang = math.atan2(dy, dx)
                for step in range(1, 90):
                    found = None
                    for sgn in (1, -1):
                        t = ang + sgn * step * math.radians(2)
                        q = clean.get((int(CX + d * math.cos(t)), int(CY + d * math.sin(t))))
                        if q and q[3] >= 230:
                            found = q
                            break
                    if found:
                        p = found
                        break
                p = [p[0], p[1], p[2], 255]
            a = p[3] * max(0.0, min(1.0, R - d + 0.5))
            out.putpixel((x, y), (p[0], p[1], p[2], int(a)))
    box = (int(CX - R - 1), int(CY - R - 1), int(math.ceil(CX + R + 1)), int(math.ceil(CY + R + 1)))
    icon = out.crop(box).resize((SIZE, SIZE), Image.LANCZOS)
    icon.save(f"{ROOT}/gfx/icon_{name}.png")
    # Greyed out until the game unlocks the item.
    grey = icon.convert("LA").convert("RGBA")
    r, g, b, a = grey.split()
    grey = Image.merge("RGBA", [c.point(lambda v: int(v * 0.6)) for c in (r, g, b)] + [a])
    grey.save(f"{ROOT}/gfx/icon_{name}_off.png")
print("gfx/icon_*.png written")
