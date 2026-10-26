# Builds game-style UI pieces for the bottom screen from a full snapshot's texture dump taken in Oak's
# Lab (snapshots_full.txt; snap_<vi>/tex_<addr>_<w>x<h>_*.rgba):
#   gfx/button.png      the lab menu's button (from "Save", 92x21 at RDRAM 0x385A90) with its label
#                       area cleared (the fill's clean column swept across) and without its flag tab
#   gfx/focus_<c>.png   the orange selection brackets (12x10 corners at 0x381938..0x381C78), named by
#                       the corner they sit in (tl, tr, bl, br), found from where their pixels are
# Usage: python -I tools/make_ui_parts.py <snapshot texture dir>
import glob
import sys

from PIL import Image

ROOT = __file__.replace("\\", "/").rsplit("/tools/", 1)[0]
d = sys.argv[1].rstrip("/\\") + "/"


def load(addr, w, h):
    return Image.frombytes("RGBA", (w, h), open(glob.glob(d + f"tex_{addr}_*.rgba")[0], "rb").read())


button = load("385A90", 92, 21)


def light(p):
    return p[3] > 0 and p[0] > 0xA0 and p[2] > 0xE0  # the label box's light inner border


# Label box: the light border's columns on row 3; its fill is rows 4-15 between them.
cols = [x for x in range(16, 92) if light(button.getpixel((x, 3)))]  # past the flag tab
left, right = cols[0], cols[-1]
clean = left + 2
for y in range(4, 16):
    p = button.getpixel((clean, y))
    for x in range(clean + 1, right - 1):
        button.putpixel((x, y), p)
# No flag tab ("lip"): keep the body around the label box, its left edge the right edge mirrored.
margin = 92 - right - 1                      # columns right of the label box
body = button.crop((left - margin, 0, 92, 21))
for x in range(margin):
    for y in range(21):
        body.putpixel((x, y), body.getpixel((body.width - 1 - x, y)))
body.save(ROOT + "/gfx/button.png")
print(f"button {body.width}x21, label box x {margin}-{body.width - 1 - margin}")

for addr in ("381938", "381A88", "381B80", "381C78"):
    c = load(addr, 12, 10)
    px = [(x, y) for y in range(10) for x in range(12) if c.getpixel((x, y))[3] > 128]
    mx = sum(x for x, _ in px) / len(px)
    my = sum(y for _, y in px) / len(px)
    name = ("t" if my < 4.5 else "b") + ("l" if mx < 5.5 else "r")
    c.save(ROOT + f"/gfx/focus_{name}.png")
    print(addr, "->", name)
print(f"gfx/button.png written (label box x {left}-{right})")
