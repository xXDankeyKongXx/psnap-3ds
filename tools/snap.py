"""Converts the app's dev screenshot (azahar/user/sdmc/3ds/psnap/screenshot.bmp) to a PNG.

Usage: python tools/snap.py [output.png]   (default: tools/tmp/screenshot.png)
"""
import sys
from pathlib import Path

from PIL import Image

root = Path(__file__).resolve().parent.parent
src = root / "azahar" / "user" / "sdmc" / "3ds" / "psnap" / "screenshot.bmp"
dst = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "tools" / "tmp" / "screenshot.png"

img = Image.open(src)
img.save(dst)
print(f"{dst} ({img.width}x{img.height})")
