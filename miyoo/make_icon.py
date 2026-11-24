#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# The Allium app icon (miyoo/pak/icon.png, 72x72): the groovebox of the start screen's picture (miyoo/art/splash.png).
# Run: python3 miyoo/make_icon.py   (needs Pillow)
from pathlib import Path
from PIL import Image

here = Path(__file__).parent
im = Image.open(here / "art" / "splash.png").convert("RGB")
crop = im.crop((200, 420, 1250, 1086))         # the groovebox, its bars and the pink floor (not the title: too small at 72 px)
side = max(crop.size)
square = Image.new("RGB", (side, side), (0, 0, 0))
square.paste(crop, ((side - crop.width) // 2, (side - crop.height) // 2))
square.resize((72, 72), Image.LANCZOS).save(here / "pak" / "icon.png")
print("icon.png: 72x72")
