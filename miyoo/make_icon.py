#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# The Allium app icon (miyoo/pak/icon.png, 72x72): the start screen's picture (miyoo/art/splash.png) without its
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
square.resize((72, 72), Image.LANCZOS).save(here / "pak" / "icon.png")
print("icon.png: 72x72")
