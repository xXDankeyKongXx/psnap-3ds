# "DANKEY KONG" in the style of the top row of the Diddy's Smash Racing logo: chunky red letters,
# each tilted and bounced a little, a thick yellow rim with an orange bevel and a deep orange 3D
# extrusion down and right, white glints on the rim.
# Usage: python make_dankey_kong.py [font.ttf] [out.png]
import math
import os
import sys
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONT = sys.argv[1] if len(sys.argv) > 1 else "C:/Windows/Fonts/ariblk.ttf"
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "dankey_kong.png")
TEXT = "DANKEY KONG"

SIZE = 300          # letter size (px)
RIM = 30            # yellow rim width
DEPTH = 18          # 3D extrusion
RED_TOP, RED_BOTTOM = (255, 52, 28), (205, 18, 14)
RED_EDGE = (150, 10, 10)
YELLOW_TOP, YELLOW_BOTTOM = (255, 246, 70), (255, 196, 0)
ORANGE = (238, 112, 0)
ORANGE_DARK = (176, 64, 0)


def vgrad(size, top, bottom):
    w, h = size
    g = Image.new("RGBA", size)
    d = ImageDraw.Draw(g)
    for y in range(h):
        t = y / max(1, h - 1)
        d.line([(0, y), (w, y)], fill=tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,))
    return g


def colored(mask, fill):
    """A layer of `fill` (colour tuple or image) through `mask` (L)."""
    if isinstance(fill, tuple):
        layer = Image.new("RGBA", mask.size, fill + (255,))
    else:
        layer = fill.copy()
    layer.putalpha(ImageChops.multiply(layer.getchannel("A"), mask))
    return layer


def letter(ch, font):
    """One letter, built up from masks: extrusion, rim, bevel, face, glints."""
    l, t, r, b = font.getbbox(ch)
    pad = RIM + DEPTH + 20
    W, H = r - l + pad * 2, b - t + pad * 2
    def mask_of(stroke, dx=0, dy=0):
        m = Image.new("L", (W, H), 0)
        ImageDraw.Draw(m).text((pad - l + dx, pad - t + dy), ch, font=font, fill=255, stroke_width=stroke, stroke_fill=255)
        return m.filter(ImageFilter.GaussianBlur(1.2)).point(lambda v: 255 if v > 110 else int(v * 255 / 110))

    face = mask_of(0)
    rim = mask_of(RIM)
    out = Image.new("RGBA", (W, H), (0, 0, 0, 0))

    # 3D extrusion: the rim stepped down-right, darker the deeper.
    for k in range(DEPTH, 0, -1):
        shift = Image.new("L", (W, H), 0)
        shift.paste(rim, (k // 2, k))
        depth = k / DEPTH
        col = tuple(int(ORANGE[i] + (ORANGE_DARK[i] - ORANGE[i]) * depth) for i in range(3))
        out.alpha_composite(colored(shift, col))
    # Rim: yellow, light at the top; a bevel - the lower right of the rim in orange.
    out.alpha_composite(colored(rim, vgrad((W, H), YELLOW_TOP, YELLOW_BOTTOM)))
    inner = mask_of(int(RIM * 0.45))
    bevel = ImageChops.subtract(rim, ImageChops.offset(rim, -5, -7))
    bevel = ImageChops.multiply(bevel, ImageChops.invert(inner)).filter(ImageFilter.GaussianBlur(3))
    out.alpha_composite(colored(bevel, ORANGE))
    # A thin orange line where the rim meets the face, then the red face with a darker edge.
    out.alpha_composite(colored(mask_of(5), ORANGE))
    out.alpha_composite(colored(face, RED_EDGE))
    face_in = face.filter(ImageFilter.MinFilter(5))
    out.alpha_composite(colored(face_in, vgrad((W, H), RED_TOP, RED_BOTTOM)))
    # Glints on the rim's upper left.
    glint = ImageChops.subtract(rim, ImageChops.offset(rim, 4, 6))
    glint = ImageChops.multiply(glint, ImageChops.invert(inner)).filter(ImageFilter.GaussianBlur(2)).point(lambda v: int(v * 0.8))
    out.alpha_composite(colored(glint, (255, 255, 235)))
    return out


def main():
    font = ImageFont.truetype(FONT, SIZE)
    # Each letter tilted and lifted a little, like the reference (alternating, with some variety).
    tilts = [-7, 4, -3, 6, -5, 3, 0, -6, 5, -4, 7]
    lifts = [8, -10, 6, -14, 4, -8, 0, 10, -12, 6, -6]
    # Letters packed so their rims touch and overlap a little (centres one advance apart).
    letters, x = [], 0.0
    for i, ch in enumerate(TEXT):
        if ch == " ":
            x += SIZE * 0.28
            continue
        adv = font.getlength(ch) + RIM * 0.55
        im = letter(ch, font)
        im = im.resize((im.width, int(im.height * 1.12)), Image.LANCZOS)  # a little taller, as in the logo
        im = im.rotate(tilts[i % len(tilts)], resample=Image.BICUBIC, expand=True)
        letters.append((im, x + adv / 2, lifts[i % len(lifts)]))
        x += adv
    W = int(x) + SIZE * 2
    H = max(im.height for im, _, _ in letters) + 80
    canvas = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    # Left to right, each over the one before (as in the logo).
    for im, cx, lift in letters:
        canvas.alpha_composite(im, (int(cx + SIZE - im.width / 2), (H - im.height) // 2 + lift))
    canvas = canvas.crop(canvas.getbbox())
    canvas.save(OUT)
    white = Image.new("RGBA", (canvas.width + 80, canvas.height + 80), (255, 255, 255, 255))
    white.alpha_composite(canvas, (40, 40))
    white.convert("RGB").save(OUT.replace(".png", "_on_white.png"))
    print(OUT, canvas.size)


if __name__ == "__main__":
    main()
