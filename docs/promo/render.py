#!/usr/bin/env python3
# MY-FAM promo: the Miyoo Mini Plus (from David's photo) turning a little in 3D, the app's frames on its screen,
# the captions. render.py yt|ig [--still T,T,...]  ->  out_yt.mp4 / out_ig.mp4 (or stills)
import math, subprocess, sys, os
import numpy as np, cv2
from PIL import Image, ImageDraw, ImageFont, ImageFilter

FMT = sys.argv[1]
W, H = (1920, 1080) if FMT == 'yt' else (1080, 1920)
FPS, T = 30, 53.0
HERE = os.path.dirname(os.path.abspath(__file__))
FR = os.path.join(HERE, 'frames')
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
FONT_B = os.path.join(ROOT, 'assets/fonts/BarlowSemiCondensed-Bold.ttf')
FONT_S = os.path.join(ROOT, 'assets/fonts/BarlowSemiCondensed-SemiBold.ttf')
FONT_M = os.path.join(ROOT, 'assets/fonts/BarlowSemiCondensed-Medium.ttf')
ORANGE = (255, 140, 30)

# ---------------------------------------------------------------- the console sprite
photo = cv2.imread(os.path.join(HERE, 'miyoo.jpg'))
gray = cv2.cvtColor(photo, cv2.COLOR_BGR2GRAY)
n, lab, st, _ = cv2.connectedComponentsWithStats((gray < 235).astype(np.uint8))
i = max(range(1, n), key=lambda k: st[k, 4] if st[k, 0] < 200 else 0)      # the front (left) console
x0, y0, cw, ch = st[i, :4]
mask = (lab == i).astype(np.uint8) * 255
# fill holes (light parts inside: the buttons' highlights)
ff = mask.copy(); h_, w_ = ff.shape
cv2.floodFill(ff, np.zeros((h_ + 2, w_ + 2), np.uint8), (0, 0), 255)
mask = mask | cv2.bitwise_not(ff)
pad = 4
crop = photo[y0 - pad:y0 + ch + pad, x0 - pad:x0 + cw + pad]
cm = mask[y0 - pad:y0 + ch + pad, x0 - pad:x0 + cw + pad]
UP = 3
sprite = cv2.resize(crop, None, fx=UP, fy=UP, interpolation=cv2.INTER_LANCZOS4)
am = cv2.resize(cv2.GaussianBlur(cm, (3, 3), 0.8), None, fx=UP, fy=UP, interpolation=cv2.INTER_LINEAR)
sprite = cv2.cvtColor(sprite, cv2.COLOR_BGR2RGB).astype(np.float32)
alpha = am.astype(np.float32) / 255.0
SH, SW = alpha.shape
# the screen: the dark glass (95,195)-(385,423) in the photo; the picture 4:3 inside it
gx0, gy0, gx1, gy1 = 95, 195, 385, 423
sw_ = (gx1 - gx0) - 12
sh_ = sw_ * 3 / 4
sx = (gx0 + 6 - x0 + pad) * UP
sy = (gy0 + ((gy1 - gy0) - sh_) / 2 - y0 + pad) * UP
SCR = np.float32([[sx, sy], [sx + sw_ * UP, sy], [sx + sw_ * UP, sy + sh_ * UP], [sx, sy + sh_ * UP]])
SPR = np.float32([[0, 0], [SW, 0], [SW, SH], [0, SH]])

# ---------------------------------------------------------------- the background
def background():
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    g = np.zeros((H, W, 3), np.float32)
    top, bot = np.array([16, 15, 22], np.float32), np.array([8, 8, 11], np.float32)
    g[:] = top + (bot - top) * (yy / H)[..., None]
    cx, cy = (W * 0.70, H * 0.5) if FMT == 'yt' else (W * 0.5, H * 0.36)
    d = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2) / (0.55 * max(W, H))
    glow = np.clip(1 - d, 0, 1) ** 2.2
    g += glow[..., None] * np.array([70, 34, 8], np.float32)
    # a faint grid
    step = 60
    grid = ((xx % step) < 1.2) | ((yy % step) < 1.2)
    g[grid] += 7
    return np.clip(g, 0, 255)
BG = background()

# ---------------------------------------------------------------- 3D
def ease(x): x = min(max(x, 0.0), 1.0); return x * x * (3 - 2 * x)
def pose(t):
    yaw = math.radians(8.0 * math.sin(2 * math.pi * t / 9.0))
    pitch = math.radians(4.0 * math.sin(2 * math.pi * t / 7.0 + 1.0))
    roll = math.radians(1.6 * math.sin(2 * math.pi * t / 11.0 + 0.5))
    fy = 10 * math.sin(2 * math.pi * t / 5.0)
    if FMT == 'yt':
        cx, cy, hgt = W * 0.70, H * 0.50 + fy, 880
    else:
        cx, cy, hgt = W * 0.50, H * 0.355 + fy, 1060
    k = ease(t / 1.4)                    # the entrance: up from below, growing
    cy += (1 - k) * 260
    hgt *= 0.82 + 0.18 * k
    if t > T - 5.5:                      # the end: a slow push in
        hgt *= 1 + 0.06 * ease((t - (T - 5.5)) / 5.5)
    return yaw, pitch, roll, cx, cy, hgt, k

def project(pts, yaw, pitch, roll, cx, cy, scale):
    # sprite points (px) -> a plane centred at the origin, rotated, seen by a camera at distance F
    F = 2600.0
    p = np.array([[(x - SW / 2) * scale, (y - SH / 2) * scale, 0.0] for x, y in pts], np.float64)
    cy_, sy_ = math.cos(yaw), math.sin(yaw)
    cp, sp = math.cos(pitch), math.sin(pitch)
    cr, sr = math.cos(roll), math.sin(roll)
    Ry = np.array([[cy_, 0, sy_], [0, 1, 0], [-sy_, 0, cy_]])
    Rx = np.array([[1, 0, 0], [0, cp, -sp], [0, sp, cp]])
    Rz = np.array([[cr, -sr, 0], [sr, cr, 0], [0, 0, 1]])
    q = p @ (Rz @ Rx @ Ry).T
    z = F + q[:, 2]
    return np.float32(np.stack([cx + q[:, 0] * F / z, cy + q[:, 1] * F / z], 1))

def warp_into(canvas, src, src_alpha, src_quad, dst_quad):
    """src (h,w,3 float) with src_alpha (h,w) onto canvas through the homography src_quad -> dst_quad"""
    x0, y0 = np.floor(dst_quad.min(0)).astype(int) - 2
    x1, y1 = np.ceil(dst_quad.max(0)).astype(int) + 2
    x0, y0 = max(x0, 0), max(y0, 0)
    x1, y1 = min(x1, W), min(y1, H)
    if x1 <= x0 or y1 <= y0:
        return None
    M = cv2.getPerspectiveTransform(src_quad, dst_quad - np.float32([x0, y0]))
    size = (int(x1 - x0), int(y1 - y0))
    rgb = cv2.warpPerspective(src, M, size, flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT)
    a = cv2.warpPerspective(src_alpha, M, size, flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT)[..., None]
    reg = canvas[y0:y1, x0:x1]
    reg[:] = reg * (1 - a) + rgb * a
    return (x0, y0, x1, y1, a)

# ---------------------------------------------------------------- text
_fonts = {}
def font(path, size):
    k = (path, size)
    if k not in _fonts:
        _fonts[k] = ImageFont.truetype(path, size)
    return _fonts[k]

SCENES = [  # start, end, kicker, title, body
    (0.0, 5.0, None, 'MY-FAM', 'A drum synth and 16-step sequencer\nfor the Miyoo Mini Plus'),
    (5.0, 11.0, '01', '16-STEP SEQUENCER', 'Parameter locks, step conditions,\nratchets, swing and nudge.'),
    (11.0, 17.0, '02', 'TWO-OSCILLATOR SYNTH', '16 wave types, sync and FM, ladder and\nstate-variable filters, three envelopes\nand an 8-cable patch bay.'),
    (17.0, 23.0, '03', 'KICK SYNTH', 'A drum voice with its own\n16-step sequence.'),
    (23.0, 29.0, '04', 'EFFECTS', 'Compressor, stutter, delay, reverb,\nDJ filter and limiter.'),
    (29.0, 35.0, '05', '176 PATTERNS', '11 banks, a sound of its own for every\npattern, and a 32-slot song.'),
    (35.0, 41.0, '06', 'RANDOM', 'New sounds, new steps, or both:\nhold L1 + R1 and press one button.'),
    (41.0, 48.0, '07', 'EXPORT WAV', 'Render a pattern or the song\ninto any folder on the SD card.'),
    (48.0, 53.0, None, 'MY-FAM', 'Free and open source  ·  Allium OS\ngithub.com/davidamomusic/my-fam'),
]

def text_layer(t):
    img = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    for (a, b, kick, title, body) in SCENES:
        if not (a <= t < b + 0.001):
            continue
        intro_or_outro = kick is None
        fin = ease((t - a - 0.15) / 0.45) if a > 0 else ease((t - 0.6) / 0.6)
        fout = 1.0 if b >= T else 1 - ease((t - (b - 0.35)) / 0.3)
        o = fin * fout
        dy = (1 - fin) * 30
        if FMT == 'yt':
            x, y = 130, (300 if intro_or_outro else 330)
            tsize = 170 if intro_or_outro else 78
            bsize = 44
        else:
            x, y = 90, (1330 if intro_or_outro else 1360)
            tsize = 170 if intro_or_outro else 84
            bsize = 46
        y += dy
        if kick:
            f = font(FONT_S, 34)
            d.rectangle([x, y - 64, x + 64, y - 22], fill=ORANGE + (int(255 * o),))
            d.text((x + 32, y - 43), kick, font=f, fill=(14, 14, 18, int(255 * o)), anchor='mm')
            d.text((x + 82, y - 43), '/ 07', font=f, fill=(140, 140, 150, int(255 * o)), anchor='lm')
        ft = font(FONT_B, tsize)
        col = ORANGE if intro_or_outro else (245, 245, 245)
        d.text((x, y), title, font=ft, fill=col + (int(255 * o),))
        th = ft.getbbox(title)[3]
        by = y + th + (26 if intro_or_outro else 30)
        if not intro_or_outro:
            d.rectangle([x, y + th + 12, x + 120, y + th + 17], fill=ORANGE + (int(255 * o),))
        d.multiline_text((x, by), body, font=font(FONT_M, bsize), fill=(200, 200, 210, int(255 * o)), spacing=12)
        if intro_or_outro:
            nb = body.count('\n') + 1
            yy = by + nb * (bsize + 12) + 30
            d.text((x, yy), 'by David Amo', font=font(FONT_S, 40), fill=(150, 150, 160, int(255 * o)))
    return img

# ---------------------------------------------------------------- a frame
def frame(t):
    k = min(int(round(t * FPS)), int(T * FPS) - 1)
    app = cv2.cvtColor(cv2.imread(os.path.join(FR, 'f%05d.png' % k)), cv2.COLOR_BGR2RGB).astype(np.float32)
    yaw, pitch, roll, cx, cy, hgt, kin = pose(t)
    scale = hgt / SH
    canvas = BG.copy()
    quad = project(SPR, yaw, pitch, roll, cx, cy, scale)
    # the shadow: a soft ellipse under it
    sh = np.zeros((H, W), np.float32)
    bx = quad[:, 0].mean(); by = quad[:, 1].max() + 18
    cv2.ellipse(sh, (int(bx), int(by)), (int(hgt * 0.36), int(hgt * 0.045)), 0, 0, 360, 1.0, -1)
    sh = cv2.GaussianBlur(sh, (0, 0), 22) * 0.75 * kin
    canvas *= (1 - sh[..., None])
    # depth: the side of the case, stacked behind in the case's colour, towards where the turn shows it
    side = np.zeros((SH, SW, 3), np.float32); side[:] = (96, 94, 88)
    depth = 26
    ddx = -math.sin(yaw) * depth; ddy = math.sin(pitch) * depth
    for j in range(6, 0, -1):
        f = j / 6
        warp_into(canvas, side * (0.75 + 0.1 * f), alpha, SPR, quad + np.float32([ddx * f, ddy * f]))
    # the console, lit a little more on the side that turns to the light
    light = 1.0 + 0.10 * math.sin(yaw) - 0.05 * math.sin(pitch)
    warp_into(canvas, np.clip(sprite * light, 0, 255), alpha, SPR, quad)
    # the app on its screen
    sq = project(SCR, yaw, pitch, roll, cx, cy, scale)
    src = np.float32([[0, 0], [640, 0], [640, 480], [0, 480]])
    r = warp_into(canvas, app, np.ones((480, 640), np.float32), src, sq)
    if r:                                # a glare across the glass, moving with the turn
        x0, y0, x1, y1, a = r
        yy, xx = np.mgrid[y0:y1, x0:x1].astype(np.float32)
        u = (xx - sq[:, 0].min()) / max(1, np.ptp(sq[:, 0])) - (yy - sq[:, 1].min()) / max(1, np.ptp(sq[:, 1])) * 0.6
        c = 0.25 + 0.25 * math.sin(yaw * 4)
        gl = np.exp(-((u - c) ** 2) / 0.02) * 0.10
        canvas[y0:y1, x0:x1] += (gl[..., None] * 255) * a
    out = Image.fromarray(np.clip(canvas, 0, 255).astype(np.uint8))
    out.alpha_composite if False else None
    out = Image.alpha_composite(out.convert('RGBA'), text_layer(t)).convert('RGB')
    # fade in from black, fade out at the very end
    fi = ease(t / 0.5); fo = 1 - ease((t - (T - 0.8)) / 0.8)
    if fi < 1 or fo < 1:
        out = Image.fromarray((np.asarray(out, np.float32) * min(fi, fo)).astype(np.uint8))
    return out

if __name__ == '__main__':
    if len(sys.argv) > 2 and sys.argv[2] == '--still':
        for ts in sys.argv[3].split(','):
            frame(float(ts)).save(os.path.join(HERE, f'still_{FMT}_{ts}.png'))
        sys.exit(0)
    # one file per scene, then joined
    parts = []
    bounds = [0, 11, 23, 35, 47, T]
    for si in range(len(bounds) - 1):
        a, b = bounds[si], bounds[si + 1]
        name = os.path.join(HERE, f'part_{FMT}_{si}.mp4')
        tmp = name + '.tmp.mp4'
        p = subprocess.Popen(['ffmpeg', '-y', '-loglevel', 'error', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{W}x{H}',
                              '-r', str(FPS), '-i', '-', '-c:v', 'libx264', '-preset', 'veryfast', '-crf', '18',
                              '-pix_fmt', 'yuv420p', '-r', str(FPS), tmp], stdin=subprocess.PIPE)
        for k in range(int(round(a * FPS)), int(round(b * FPS))):
            p.stdin.write(frame(k / FPS).tobytes())
        p.stdin.close()
        if p.wait() != 0:
            sys.exit('ffmpeg failed')
        os.replace(tmp, name)
        parts.append(name)
        print('scene', si, 'done', flush=True)
    lst = os.path.join(HERE, f'list_{FMT}.txt')
    open(lst, 'w').write(''.join(f"file '{q}'\n" for q in parts))
    silent = os.path.join(HERE, f'silent_{FMT}.mp4')
    subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-f', 'concat', '-safe', '0', '-i', lst, '-c', 'copy', silent], check=True)
    final = os.path.join(HERE, f'MY-FAM_promo_{"youtube_16x9" if FMT == "yt" else "reel_9x16"}.mp4')
    subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-i', silent, '-i', os.path.join(FR, 'app.wav'),
                    '-map', '0:v', '-map', '1:a', '-c:v', 'copy', '-af', f'afade=t=in:d=0.3,afade=t=out:st={T - 2.0}:d=2.0',
                    '-c:a', 'aac', '-b:a', '192k', '-shortest', '-movflags', '+faststart', final], check=True)
    print(final)
