# The promo video

How `MY-FAM_promo_youtube_16x9.mp4` and `MY-FAM_promo_reel_9x16.mp4` (next to this file) were made.

1. `make_script.py` writes `app_script.txt`: a script for the test runner with the demo's button presses at their
   times and a screenshot every 1/30 s. Run it in `docs/promo/frames/` with the runner
   (`python3 tools/build.py fam --miyoo-test`) to get `f00000.png` ... and `app.wav` (the app's own sound).
   The WAV export scene expects `/mnt/SDCARD` with a few folders (Apps, BIOS, Music, ...).
2. `render.py yt` / `render.py ig` puts the frames on the console's screen, turns it a little in 3D, adds the captions
   and the sound (Python 3 with numpy, OpenCV, Pillow; ffmpeg).

The photo of the console (`miyoo.jpg`, the front of a Miyoo Mini Plus on white, 800×800) is not in the repo.
