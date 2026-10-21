#!/usr/bin/env python3
# replay-side-by-side.py <work folder>: what the player and the live watcher saw, side by side, from
# replay-check.py --video Metal --shots MS (user-Host/ScreenShots h-*.png, user-Watcher/ScreenShots
# w-*.png, taken at the same instants). Writes side-by-side.mp4 and a contact sheet
# side-by-side.png into the work folder.
import os
import subprocess
import sys

from PIL import Image, ImageDraw

work = sys.argv[1]
host_dir = os.path.join(work, "user-Host", "ScreenShots")
watch_dir = os.path.join(work, "user-Watcher", "ScreenShots")
names = sorted(f[2:] for f in os.listdir(host_dir) if f.startswith("h-") and f.endswith(".png"))
pairs = [(os.path.join(host_dir, "h-" + n), os.path.join(watch_dir, "w-" + n)) for n in names
         if os.path.exists(os.path.join(watch_dir, "w-" + n))]
if not pairs:
    sys.exit("no matching screenshots")
frames_dir = os.path.join(work, "sbs")
os.makedirs(frames_dir, exist_ok=True)
W, H = 640, 480
for i, (h, w) in enumerate(pairs):
    canvas = Image.new("RGB", (W * 2 + 12, H + 40), (18, 18, 20))
    for j, (path, label) in enumerate(((h, "Player (Host's screen)"), (w, "Live watcher (replaying the stream)"))):
        img = Image.open(path).convert("RGB")
        img.thumbnail((W, H))
        x = j * (W + 12) + (W - img.width) // 2
        canvas.paste(img, (x, 40 + (H - img.height) // 2))
        ImageDraw.Draw(canvas).text((j * (W + 12) + 8, 12), label, fill=(240, 240, 240))
    canvas.save(os.path.join(frames_dir, f"{i:04d}.png"))
mp4 = os.path.join(work, "side-by-side.mp4")
subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", sys.argv[2] if len(sys.argv) > 2 else "10", "-i",
                os.path.join(frames_dir, "%04d.png"), "-pix_fmt", "yuv420p", "-vf",
                "scale=trunc(iw/2)*2:trunc(ih/2)*2", mp4], check=True)
step = max(1, len(pairs) // 6)
picks = list(range(0, len(pairs), step))[:6]
sheet = Image.new("RGB", (W * 2 + 12, (H + 40) * len(picks)), (18, 18, 20))
for k, i in enumerate(picks):
    sheet.paste(Image.open(os.path.join(frames_dir, f"{i:04d}.png")), (0, k * (H + 40)))
sheet.save(os.path.join(work, "side-by-side.png"))
print(f"{len(pairs)} pairs -> {mp4} and side-by-side.png")
