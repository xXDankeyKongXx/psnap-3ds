# Builds the HOME menu art for the CIA:
#   assets/banner/icon.png     48x48   Todd's picture from his PKMN photographer card (name entry)
#   assets/banner/banner.png  256x128  a 3D rendering of the Pokemon Snap logo, with the port's "Double
#                                      Take!!" film strip under it, over a photo of Pokemon Island tilted
#                                      to the right
#   assets/banner/banner.wav           the start of the title screen music (3 s)
# Inputs (captured from the port in Azahar):
#   assets/banner/todd_card.png   the card's portrait (66x81, VI 1800 of a new game)
#   assets/banner/logo.png        the title logo, keyed out of the title screen against the island
#                                 frame before it appears (251x74 with the TM)
#   gfx/island.png                the island the title screen shows (320x240)
#   assets/banner/title_music.raw the title music's first 3 s, recorded from the game's output
#                                 (pokes "audiorec"; 32000 Hz stereo 16-bit, from 0.3 s after VI 1380)
# The logo is extruded: copies stepped back and down, darkened with depth (a solid body), its face
# bevelled (light from the top left) with a gloss over the upper half, leaned back in perspective and
# dropping a soft shadow onto the photo.
# Run: python -I tools/make_banner.py
import math
import struct
import wave

from PIL import Image, ImageChops, ImageDraw, ImageEnhance, ImageFilter

ROOT = __file__.replace("\\", "/").rsplit("/tools/", 1)[0]
A = ROOT + "/assets/banner/"

# --- Icon: Todd ------------------------------------------------------------------------------------
card = Image.open(A + "todd_card.png").convert("RGB")
icon = card.crop((1, 3, 65, 67)).resize((48, 48), Image.LANCZOS)
icon = ImageEnhance.Sharpness(icon).enhance(1.3)
icon.save(A + "icon.png")
icon.save(ROOT + "/icon.png")  # the SMDH icon (Makefile)

# --- Banner: 3D logo ----------------------------------------------------------------------------------
S = 4  # work at 4x, scale down at the end
W, H = 256 * S, 128 * S

logo = Image.open(A + "logo.png").convert("RGBA")
logo = logo.resize((logo.width * S, logo.height * S), Image.LANCZOS)
# Keep only the logo (the texture's empty corners can hold a little stray colour).
alpha = logo.getchannel("A").point(lambda a: 255 if a > 96 else 0).filter(ImageFilter.GaussianBlur(0.8 * S / 2))
logo.putalpha(alpha)

# Extrusion: a solid body in each word's outline colour (deep blue under "Pokemon", deep red under
# "Snap", which starts at x 133 of the logo), stepping back and down, darker further back.
depth = 9 * S
split = 133 * S
colour = Image.new("RGBA", logo.size, (28, 52, 140, 255))
ImageDraw.Draw(colour).rectangle([split, 0, logo.width, logo.height], fill=(150, 34, 16, 255))
body = Image.new("RGBA", (logo.width + depth, logo.height + depth), (0, 0, 0, 0))
for i in range(depth, 0, -1):
    shade = 0.45 + 0.55 * (1 - i / depth)
    layer = ImageEnhance.Brightness(colour).enhance(shade)
    layer.putalpha(logo.getchannel("A"))
    body.alpha_composite(layer, (int(i * 0.55), i))

# Face: bevel from the alpha (light from the top left), gloss on the upper half.
a = logo.getchannel("A")
blur = a.filter(ImageFilter.GaussianBlur(2.2 * S / 2))
hi = ImageChops.subtract(blur, ImageChops.offset(blur, S, S))       # edges facing the light
lo = ImageChops.subtract(blur, ImageChops.offset(blur, -S, -S))     # edges facing away
face = logo.copy()
white = Image.new("RGBA", logo.size, (255, 255, 255, 255))
black = Image.new("RGBA", logo.size, (0, 0, 0, 255))
face = Image.composite(white, face, hi.point(lambda v: min(255, v * 3)).point(lambda v: v * 0.55))
face = Image.composite(black, face, lo.point(lambda v: min(255, v * 3)).point(lambda v: v * 0.35))
gloss = Image.new("L", logo.size, 0)
gd = ImageDraw.Draw(gloss)
for y in range(logo.height // 2):
    gd.line([(0, y), (logo.width, y)], fill=int(70 * (1 - y / (logo.height / 2))))
face = Image.composite(white, face, ImageChops.multiply(gloss, a))
face.putalpha(a)
body.alpha_composite(face, (0, 0))

# Lean back: a perspective warp, the top narrower than the bottom.
bw, bh = body.size
inset = bw * 0.06
coeffs = None


def perspective_coeffs(src, dst):
    # Solve for the 8 coefficients mapping output (dst) points to input (src) points.
    import numpy as np
    m = []
    for (x, y), (u, v) in zip(dst, src):
        m.append([x, y, 1, 0, 0, 0, -u * x, -u * y])
        m.append([0, 0, 0, x, y, 1, -v * x, -v * y])
    b = [c for p in src for c in p]
    return np.linalg.solve(np.array(m, float), np.array(b, float)).tolist()


dst = [(inset, 0), (bw - inset, 0), (bw, bh), (0, bh)]
src = [(0, 0), (bw, 0), (bw, bh), (0, bh)]
body = body.transform((bw, bh), Image.PERSPECTIVE, perspective_coeffs(src, dst), Image.BICUBIC)

# Background: a sky-blue gradient, and on it a photo of the island (white print border) tilted to
# the right, casting a soft shadow.
bg = Image.new("RGBA", (W, H))
gd = ImageDraw.Draw(bg)
for y in range(H):
    t = y / H
    gd.line([(0, y), (W, y)], fill=(int(60 + 90 * t), int(140 + 70 * t), int(230 + 20 * t), 255))
island = Image.open(ROOT + "/gfx/island.png").convert("RGB")
island = ImageEnhance.Color(ImageEnhance.Brightness(island).enhance(1.05)).enhance(1.15)
pw = int(W * 0.78)
ph = int(pw * 0.75)
border = int(pw * 0.045)
photo = Image.new("RGBA", (pw + 2 * border, ph + 3 * border), (250, 250, 246, 255))  # print, deeper at the bottom
photo.paste(island.resize((pw, ph), Image.LANCZOS), (border, border))
tilted = photo.rotate(-9, resample=Image.BICUBIC, expand=True)  # clockwise: leaning to the right
pshadow = Image.new("RGBA", tilted.size, (0, 0, 0, 0))
pshadow.putalpha(tilted.getchannel("A").point(lambda v: v * 0.45))
pshadow = pshadow.filter(ImageFilter.GaussianBlur(4 * S))
px, py = (W - tilted.width) // 2 + 6 * S, (H - tilted.height) // 2 + 10 * S
bg.alpha_composite(pshadow, (px + 3 * S, py + 4 * S))
bg.alpha_composite(tilted, (px, py))

# Place the logo: most of the banner's width, in the upper part (the subtitle goes under it).
scale = (W * 0.88) / bw
body = body.resize((int(bw * scale), int(bh * scale)), Image.LANCZOS)
x, y = (W - body.width) // 2, int(H * 0.36 - body.height / 2)
shadow = Image.new("RGBA", body.size, (0, 0, 0, 0))
shadow.putalpha(body.getchannel("A").point(lambda v: v * 0.55))
shadow = shadow.filter(ImageFilter.GaussianBlur(5 * S))
bg.alpha_composite(shadow, (x + 3 * S, y + 6 * S))
bg.alpha_composite(body, (x, y))

# The port's subtitle, "Double Take!!" (tools/make_subtitle.py): a strip of film under "Snap", as on
# the title screen.
sub = Image.open(A + "subtitle.png").convert("RGBA")
sw = int(W * 0.66)
sub = sub.resize((sw, sw * sub.height // sub.width), Image.LANCZOS)
sx, sy = x + body.width - sw + int(W * 0.02), y + body.height - int(sub.height * 0.30)
bg.alpha_composite(sub, (sx, sy))

banner = bg.resize((256, 128), Image.LANCZOS).convert("RGB")
ImageEnhance.Sharpness(banner).enhance(1.2).save(A + "banner.png")

# --- Banner sound: the title music's opening ------------------------------------------------------------
import numpy as np

src_rate, rate = 32000, 22050
music = np.frombuffer(open(A + "title_music.raw", "rb").read(), dtype="<i2").reshape(-1, 2).astype(np.float64)
mono = music.mean(axis=1)
t_out = np.arange(int(len(mono) * rate / src_rate)) * (src_rate / rate)
mono = np.interp(t_out, np.arange(len(mono)), mono)
fade_in, fade_out = int(rate * 0.01), int(rate * 0.4)
mono[:fade_in] *= np.linspace(0, 1, fade_in)
mono[-fade_out:] *= np.linspace(1, 0, fade_out)
mono *= min(1.0, 30000 / max(1.0, np.abs(mono).max()))  # as loud as it can be without clipping
with wave.open(A + "banner.wav", "wb") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(rate)
    w.writeframes(mono.astype("<i2").tobytes())
print("icon.png, banner.png, banner.wav written")
