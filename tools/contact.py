"""Contact sheet of the app's snapshot screenshots (top screen) for quick review.
Usage: python -I tools/contact.py <first_vi> <step> <last_vi> [out.png]"""
import sys
from pathlib import Path
from PIL import Image, ImageDraw
root = Path(__file__).resolve().parent.parent
d = root / "azahar" / "user" / "sdmc" / "3ds" / "psnap"
a, s, b = (int(x) for x in sys.argv[1:4])
out = sys.argv[4] if len(sys.argv) > 4 else str(root / "tools" / "tmp" / "contact.png")
ims = []
for v in range(a, b + 1, s):
    p = d / f"snap_{v}.bmp"
    if p.exists():
        ims.append((v, Image.open(p).crop((40, 0, 360, 240))))
cols, w, h = 5, 160, 120
sheet = Image.new("RGB", (cols * w, max(1, (len(ims) + cols - 1) // cols) * h))
for i, (v, im) in enumerate(ims):
    im = im.resize((w, h))
    ImageDraw.Draw(im).text((2, 2), str(v), fill=(255, 255, 0))
    sheet.paste(im, ((i % cols) * w, (i // cols) * h))
sheet.save(out)
print(out, len(ims))
