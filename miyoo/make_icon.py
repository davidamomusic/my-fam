#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# The Allium app icon (miyoo/pak/icon.png, 1086x1086: the picture at its own resolution, Allium scales it): the start screen's picture (miyoo/art/splash.png) without its
# version number, the title centred, cut square. Run: python3 miyoo/make_icon.py   (needs Pillow)
from pathlib import Path
from PIL import Image, ImageDraw

here = Path(__file__).parent
im = Image.open(here / "art" / "splash.png").convert("RGB")      # 1448 x 1086
title = im.crop((100, 110, 1080, 335))                           # "My-FAM" (its white: x 127..1068)
ImageDraw.Draw(im).rectangle([100, 110, 1360, 335], fill=(0, 0, 0))   # the title and the "v16" out
im.paste(title, (100 + (724 - 598), 110))                        # back, centred on the picture's middle (x 724)
left = (im.width - im.height) // 2
square = im.crop((left, 0, left + im.height, im.height))          # the middle 1086 x 1086
# in the picture's colours (black, cream, pink and the steps between them): crisp and small
BLACK, CREAM, PINK = (0, 0, 0), (253, 247, 239), (246, 4, 130)
def mix(a, b, t):
    return tuple(round(a[k] + (b[k] - a[k]) * t) for k in range(3))
cols = [BLACK, CREAM, PINK]
for a, b in ((BLACK, CREAM), (BLACK, PINK), (CREAM, PINK)):
    cols += [mix(a, b, 1 / 3), mix(a, b, 2 / 3)]
pal = Image.new("P", (1, 1))
pal.putpalette([v for c in cols + [BLACK] * (16 - len(cols)) for v in c])
square.quantize(palette=pal, dither=Image.NONE).save(here / "pak" / "icon.png", optimize=True)
print("icon.png: %dx%d" % square.size)
