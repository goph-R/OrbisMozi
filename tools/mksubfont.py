#!/usr/bin/env python3
"""Renders the subtitle font into src/subfont.bin (anti-aliased, two sizes).

    tools/mksubfont.py [font.ttf] [out.bin]

Layout of the file (little endian):
    "MZF1", u16 number of fonts
    per font:   u16 pixel size, u16 line height, u16 ascent, u16 glyph count, u32 offset of glyph table
    per glyph:  u16 code point, u8 width, u8 height, i8 x offset, i8 y offset (from the top of the line),
                u8 advance, u8 unused, u32 offset of width*height coverage bytes
Glyph tables are sorted by code point.
"""
import struct
import sys
from PIL import Image, ImageDraw, ImageFont

ttf = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
out = sys.argv[2] if len(sys.argv) > 2 else "src/subfont.bin"

SIZES = [34, 51]            # for 720-line and 1080-line screens
CODES = (list(range(0x20, 0x7F)) + list(range(0xA0, 0x180)) +
         [0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x201E, 0x2022, 0x2026, 0x266A, 0x266B])

fonts = []
for size in SIZES:
    font = ImageFont.truetype(ttf, size)
    ascent, descent = font.getmetrics()
    glyphs = []
    for code in sorted(CODES):
        ch = chr(code)
        advance = int(round(font.getlength(ch)))
        box = font.getbbox(ch)
        w, h = (box[2] - box[0], box[3] - box[1]) if box else (0, 0)
        data = b""
        if w > 0 and h > 0:
            img = Image.new("L", (w, h), 0)
            ImageDraw.Draw(img).text((-box[0], -box[1]), ch, font=font, fill=255)
            data = img.tobytes()
        else:
            w = h = 0
            box = (0, 0, 0, 0)
        assert w < 256 and h < 256 and advance < 256 and -128 <= box[0] < 128 and -128 <= box[1] < 128
        glyphs.append((code, w, h, box[0], box[1], advance, data))
    fonts.append((size, ascent + descent, ascent, glyphs))

header_size = 6 + 12 * len(fonts)
tables, blobs = [], []
pos = header_size
table_offsets = []
for size, line_h, ascent, glyphs in fonts:
    table_offsets.append(pos)
    pos += 12 * len(glyphs)
for size, line_h, ascent, glyphs in fonts:
    table = b""
    for code, w, h, xo, yo, adv, data in glyphs:
        table += struct.pack("<HBBbbBBI", code, w, h, xo, yo, adv, 0, pos)
        blobs.append(data)
        pos += len(data)
    tables.append(table)

with open(out, "wb") as f:
    f.write(b"MZF1" + struct.pack("<H", len(fonts)))
    for (size, line_h, ascent, glyphs), off in zip(fonts, table_offsets):
        f.write(struct.pack("<HHHHI", size, line_h, ascent, len(glyphs), off))
    for t in tables:
        f.write(t)
    for b in blobs:
        f.write(b)
print(f"{out}: {pos} bytes, {len(CODES)} glyphs x {len(SIZES)} sizes")
