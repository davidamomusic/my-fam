#!/usr/bin/env python3
"""MY-FAM's start screen: miyoo/art/splash.png -> miyoo/splash.h (640x480, 16 colours, run-length coded).

The picture's own version number (top right) is painted out: the app draws the current one there
(SPLASH_VER_X / SPLASH_VER_Y), so the start screen never shows an old version.
Run: python3 miyoo/make_splash.py   (needs Pillow)
"""
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 640, 480
VER_BOX = (484, 70, 604, 142)      # where the picture's "v16" is, at 640x480: painted black
VER_X, VER_Y = 482, 78             # where the app writes "V" + MYFAM_VERSION (5x7 font at scale 8)

im = Image.open(os.path.join(HERE, "art", "splash.png")).convert("RGB")
im = im.resize((W, H), Image.BOX)
ImageDraw.Draw(im).rectangle(VER_BOX, fill=(0, 0, 0))
# the picture's three colours and the steps between them (for the soft edges): 9 of the 16
BLACK, CREAM, PINK = (0, 0, 0), (253, 247, 239), (246, 4, 130)
def mix(a, b, t):
    return tuple(round(a[k] + (b[k] - a[k]) * t) for k in range(3))
cols = [BLACK, CREAM, PINK]
for a, b in ((BLACK, CREAM), (BLACK, PINK), (CREAM, PINK)):
    cols += [mix(a, b, 1 / 3), mix(a, b, 2 / 3)]
cols += [BLACK] * (16 - len(cols))
pimg = Image.new("P", (1, 1))
pimg.putpalette([v for c in cols for v in c])
q = im.quantize(palette=pimg, dither=Image.NONE)
pal = q.getpalette()[:48]
data = list(q.tobytes())

# runs: one byte each, high nibble = run length - 1 (1..16), low nibble = colour
out, i = [], 0
while i < len(data):
    c, n = data[i], 1
    while i + n < len(data) and data[i + n] == c and n < 16:
        n += 1
    out.append((n - 1) << 4 | c)
    i += n

with open(os.path.join(HERE, "splash.h"), "w") as f:
    f.write("/* MY-FAM's start screen, from miyoo/art/splash.png by miyoo/make_splash.py: do not edit */\n")
    f.write(f"#define SPLASH_W {W}\n#define SPLASH_H {H}\n")
    f.write(f"#define SPLASH_VER_X {VER_X}\n#define SPLASH_VER_Y {VER_Y}\n")
    f.write("static const uint32_t SPLASH_PAL[16] = {")
    f.write(", ".join("0x%06X" % (pal[3 * k] << 16 | pal[3 * k + 1] << 8 | pal[3 * k + 2]) for k in range(16)))
    f.write("};\n")
    f.write(f"static const uint8_t SPLASH_RLE[{len(out)}] = {{\n")
    for k in range(0, len(out), 24):
        f.write("    " + ",".join(str(v) for v in out[k:k + 24]) + ",\n")
    f.write("};\n")
print(f"splash.h: {len(out)} bytes of runs for {W}x{H}")
