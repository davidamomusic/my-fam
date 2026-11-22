#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# The Allium app icon (miyoo/pak/icon.png, 72x72) in the side panel's 5x7 font: python3 miyoo/make_icon.py
import re, sys
from pathlib import Path
from PIL import Image, ImageDraw
here = Path(__file__).parent
rows = re.findall(r'"((?:[^"\\]|\\.)*)"', (here / "font5x7.h").read_text().split("FONT5X7[] = {")[1])
glyph = {rows[i]: rows[i + 1:i + 8] for i in range(0, len(rows) - 7, 8)}
im = Image.new("RGB", (72, 72), (20, 20, 26))
d = ImageDraw.Draw(im)
d.rectangle([0, 0, 71, 71], outline=(255, 140, 30), width=2)
def text(x, y, s, sc, col):
    for ch in s:
        for j, row in enumerate(glyph[ch]):
            for i, c in enumerate(row):
                if c == "#":
                    d.rectangle([x + i * sc, y + j * sc, x + i * sc + sc - 1, y + j * sc + sc - 1], fill=col)
        x += 6 * sc
text(12, 10, "MY", 4, (255, 140, 30))
text(10, 46, "FAM", 3, (235, 235, 235))
im.save(here / "pak" / "icon.png")
