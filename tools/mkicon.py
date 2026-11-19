#!/usr/bin/env python3
"""Draws the 512x512 home screen icon (sce_sys/icon0.png)."""
import sys
from PIL import Image, ImageDraw, ImageFont

out = sys.argv[1] if len(sys.argv) > 1 else "sce_sys/icon0.png"
bold = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

img = Image.new("RGB", (512, 512), (18, 20, 44))
d = ImageDraw.Draw(img)
d.rounded_rectangle((76, 56, 436, 336), radius=34, fill=(36, 42, 92), outline=(120, 140, 255), width=8)
d.polygon([(214, 136), (214, 256), (318, 196)], fill=(255, 255, 255))

small = ImageFont.truetype(bold, 40)
big = ImageFont.truetype(bold, 84)
d.text((256, 388), "ORBIS", font=small, fill=(120, 140, 255), anchor="mm")
d.text((256, 448), "MOZI", font=big, fill=(255, 255, 255), anchor="mm")
img.save(out)
