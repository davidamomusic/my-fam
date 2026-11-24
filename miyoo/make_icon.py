#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# The Allium app icon (miyoo/pak/icon.png, 72x72): "MY-FAM" in the app's 5x7 font over the groovebox of the start
# screen's picture (miyoo/art/splash.png), in its colours. Run: python3 miyoo/make_icon.py   (needs Pillow)
import re
from pathlib import Path
from PIL import Image, ImageDraw

here = Path(__file__).parent
CREAM, PINK, BLACK = (253, 247, 239), (246, 4, 130), (0, 0, 0)
rows = re.findall(r'"((?:[^"\\]|\\.)*)"', (here / "font5x7.h").read_text().split("FONT5X7[] = {")[1])
glyph = {rows[i]: rows[i + 1:i + 8] for i in range(0, len(rows) - 7, 8)}

icon = Image.new("RGB", (72, 72), BLACK)
d = ImageDraw.Draw(icon)

# the groovebox, its bars and the floor: the lower part
art = Image.open(here / "art" / "splash.png").convert("RGB")
box = art.crop((150, 425, 1300, 1086))                 # 1150 x 661
w = 72
h = round(box.height * w / box.width)
icon.paste(box.resize((w, h), Image.LANCZOS), (0, 72 - h))   # (41 px: from y 31)

# the name on top, on black, with the picture's pink stripes under it
d.rectangle([0, 0, 71, 29], fill=BLACK)
def text(x, y, s, sx, sy, col):                         # bold: each dot one column wider
    for ch in s:
        for j, row in enumerate(glyph[ch]):
            for i, c in enumerate(row):
                if c == "#":
                    d.rectangle([x + i * sx, y + j * sy, x + i * sx + sx, y + j * sy + sy - 1], fill=col)
        x += 6 * sx
text(1, 2, "MY-FAM", 2, 3, CREAM)                       # 6 glyphs x 12 px = 71 px, 21 px tall
d.rectangle([3, 25, 68, 25], fill=PINK)
d.rectangle([3, 27, 68, 27], fill=PINK)
icon.save(here / "pak" / "icon.png")
print("icon.png: 72x72")
