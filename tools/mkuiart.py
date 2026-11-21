#!/usr/bin/env python3
"""Packs the menu's pictures (assets/ui/*.png) into src/uiart.bin.

    tools/mkuiart.py [assets-dir] [out.bin]

The PNGs are drawn for a 1920x1080 screen; the menu runs at 1280x720, so
they are scaled by 2/3 here. The order below is the order of the UI_IMG_*
names in src/ui.h.

Layout of the file (little endian):
    "MZA1", u16 number of pictures
    per picture: u16 width, u16 height, u32 offset of width*height*4 bytes
                 (R, G, B, A with the colour already multiplied by alpha)
"""
import struct
import sys
from PIL import Image

NAMES = ["logo", "drive", "share", "plus", "btn-cross", "btn-circle", "btn-square", "btn-triangle"]

src = sys.argv[1] if len(sys.argv) > 1 else "assets/ui"
out = sys.argv[2] if len(sys.argv) > 2 else "src/uiart.bin"

pictures = []
for name in NAMES:
    img = Image.open(f"{src}/{name}.png").convert("RGBA")
    w, h = round(img.width * 2 / 3), round(img.height * 2 / 3)
    # Scaling with the colour multiplied by alpha keeps the edges clean.
    small = img.convert("RGBa").resize((w, h), Image.LANCZOS)
    pictures.append((w, h, small.tobytes()))

pos = 6 + 8 * len(pictures)
with open(out, "wb") as f:
    f.write(b"MZA1" + struct.pack("<H", len(pictures)))
    for w, h, data in pictures:
        f.write(struct.pack("<HHI", w, h, pos))
        pos += len(data)
    for w, h, data in pictures:
        f.write(data)
print(f"{out}: {pos} bytes, {len(pictures)} pictures")
