# Builds gfx/film_strip.png, a seamless strip of film for the bottom screen's course name plate, from
# the film counter's art (gfx/film_head.png: canister in columns 0-11, then strip). The strip's
# interior is tiled forwards and mirrored (so every join matches), smoothed a little, and its sprocket
# holes (rows 4 and 15, every 4 pixels) drawn again evenly along the whole length.
# Run: python -I tools/make_film.py
from PIL import Image, ImageFilter

ROOT = __file__.replace("\\", "/").rsplit("/tools/", 1)[0]
head = Image.open(ROOT + "/gfx/film_head.png").convert("RGBA")
LENGTH = 72

src = head.crop((12, 0, 44, 18))
# Holes out of the interior: each hole pixel takes the mean of its left and right neighbours.
for y in (4, 15):
    for x in range(src.width):
        if src.getpixel((x, y))[:3] == (255, 255, 255):
            l = src.getpixel((max(0, x - 1), y))
            r = src.getpixel((min(src.width - 1, x + 1), y))
            src.putpixel((x, y), tuple((a + b) // 2 for a, b in zip(l, r)))

strip = Image.new("RGBA", (LENGTH, 18))
x = 0
flip = False
while x < LENGTH:
    piece = src.transpose(Image.FLIP_LEFT_RIGHT) if flip else src
    strip.paste(piece, (x, 0))
    x += src.width
    flip = not flip

# Smooth the interior (rows 3-16) along the strip, then the border rows and holes on top.
inner = strip.crop((0, 3, LENGTH, 17)).filter(ImageFilter.GaussianBlur(1.0))
strip.paste(inner, (0, 3))
for x in range(LENGTH):
    for y in (0, 1):
        strip.putpixel((x, y), head.getpixel((20, y)))
    strip.putpixel((x, 2), (0, 0, 0, 255))
    strip.putpixel((x, 17), (0, 0, 0, 255))
    if x % 4 == 2:
        strip.putpixel((x, 4), (255, 255, 255, 255))
        strip.putpixel((x, 15), (255, 255, 255, 255))
strip.save(ROOT + "/gfx/film_strip.png")
print("gfx/film_strip.png written")
