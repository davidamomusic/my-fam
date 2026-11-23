#!/usr/bin/env python3
# the app's side of the promo: a script for the test runner, a screenshot every 1/30 s and one WAV, with the presses
# of the demo at their times (ms)
FPS = 30
T = 53.0
ev = []          # (ms, 'down'|'up', button)
def tap(t, b, hold=60):
    ev.append((t, 'down', b)); ev.append((t + hold, 'up', b))
def chord(t, mods, b, hold=60, pre=80, post=120):
    for m in mods: ev.append((t - pre, 'down', m))
    tap(t, b, hold)
    for m in mods: ev.append((t + hold + post, 'up', m))
def knob(t, face, d, n, gap=170):
    ev.append((t - 60, 'down', face))
    for i in range(n): tap(t + i * gap, d, 50)
    ev.append((t + n * gap + 40, 'up', face))

tap(250, 'START')                                   # S0 0-5: SEQ playing
# S1 5-11: notes into the steps: L2 held, X taps and moves on
ev.append((6000, 'down', 'L2'))
for i, t in enumerate([6400, 6800, 7200, 7600, 8000]):
    tap(t, 'X', 60)
    if i in (1, 3):
        tap(t + 150, 'RIGHT', 50)
ev.append((8800, 'up', 'L2'))
# S2 11-17: SYNTH, the filter page, a cutoff sweep, resonance
chord(11000, ['SELECT'], 'RIGHT')
tap(12000, 'DOWN')
knob(12800, 'Y', 'UP', 8, 160)
knob(14400, 'X', 'UP', 2, 220)
knob(15400, 'Y', 'DOWN', 6, 160)
# S3 17-23: KICK: its sequence, then its tone
chord(17000, ['SELECT'], 'DOWN')
tap(19300, 'DOWN'); tap(19700, 'DOWN'); tap(20100, 'DOWN')
knob(20900, 'X', 'UP', 5, 150)
# S4 23-29: FX: delay and reverb up
chord(23000, ['SELECT'], 'A')
tap(24000, 'DOWN')
knob(24700, 'B', 'UP', 3, 220)
tap(26300, 'DOWN')
knob(26900, 'B', 'UP', 3, 220)
# S5 29-35: PATTERNS
chord(29000, ['SELECT'], 'B')
tap(31500, 'RIGHT'); tap(32500, 'RIGHT'); tap(33500, 'LEFT'); tap(34200, 'LEFT')
# S6 35-41: RANDOM: on the SYNTH page, L1 + R1 held, A (sound)
chord(35000, ['SELECT'], 'RIGHT')
ev.append((35900, 'down', 'L1')); ev.append((35960, 'down', 'R1'))
tap(36800, 'A', 80)
tap(38800, 'A', 80)
ev.append((40300, 'up', 'R1')); ev.append((40350, 'up', 'L1'))
# S7 41-48: WAV export: R2 + X, the folder picker, Music, START
chord(41300, ['R2'], 'X')
for i, t in enumerate([42300, 42650, 43000, 43350]):
    tap(t, 'DOWN', 50)
tap(44000, 'A')
tap(45000, 'START')
# S8 48-53: playing again
chord(48600, ['SELECT'], 'UP')
tap(49200, 'START')

ev.sort(key=lambda e: e[0])
out = ['wait 1', 'wav app.wav']
cur = 0
k = 0
i = 0
nfr = int(T * FPS)
for k in range(nfr):
    target = round(k * 1000 / FPS) + 2
    while i < len(ev) and ev[i][0] <= target:
        t, what, b = ev[i]
        if t > cur:
            out.append(f'wait {t - cur}'); cur = t
        out.append(f'{what} {b}'); cur += 1
        i += 1
    if target > cur:
        out.append(f'wait {target - cur}'); cur = target
    out.append(f'shot f{k:05d}.png')
out.append('wavend')
open('app_script.txt', 'w').write('\n'.join(out) + '\n')
print(nfr, 'frames', cur, 'ms')
