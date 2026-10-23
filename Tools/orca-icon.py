#!/usr/bin/env python3
# Draws Orca's placeholder icon: a white orca dorsal fin over a wave on YouGame's red tile.
# Writes Data/Orca.png (1024 px), Data/Orca.icns (macOS) and Installer/Orca.ico (Windows).
# Needs Pillow; the .icns step needs macOS's iconutil.
#
#   python3 Tools/orca-icon.py

import math
import os
import shutil
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RED = (255, 0, 0, 255)  # YouGame's red (web/public/icons)
WHITE = (255, 255, 255, 255)
SCALE = 4  # drawn at 4096 px, then reduced, for smooth edges
SIZE = 1024


def cubic(p0, p1, p2, p3, steps=64):
    """Points along a cubic Bezier curve."""
    points = []
    for i in range(steps + 1):
        t = i / steps
        u = 1 - t
        points.append((u**3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t**3 * p3[0],
                       u**3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t**3 * p3[1]))
    return points


def scaled(points):
    return [(x * SCALE, y * SCALE) for x, y in points]


def draw():
    image = Image.new("RGBA", (SIZE * SCALE, SIZE * SCALE), (0, 0, 0, 0))
    pen = ImageDraw.Draw(image)

    # The tile, on the macOS icon grid (824 px with rounded corners, centred).
    pen.rounded_rectangle([100 * SCALE, 100 * SCALE, 924 * SCALE, 924 * SCALE], radius=185 * SCALE,
                          fill=RED)

    # The fin: a convex leading edge up to a tip that leans back, a concave trailing edge down.
    # Its base sinks into the wave, so the two read as one shape.
    base_left, tip, base_right = (318, 724), (628, 236), (734, 724)
    leading = cubic(base_left, (410, 570), (520, 320), tip)
    trailing = cubic(tip, (586, 390), (604, 600), base_right)
    pen.polygon(scaled(leading + trailing[1:]), fill=WHITE)

    # The water: a band with a wavy top and bottom, round at both ends.
    def wave_y(x):
        return 718 + 16 * math.sin((x - 212) / 600 * 2 * math.pi * 2.5)

    half = 22
    xs = [212 + i * (600 / 240) for i in range(241)]
    band = [(x, wave_y(x) - half) for x in xs] + [(x, wave_y(x) + half) for x in reversed(xs)]
    pen.polygon(scaled(band), fill=WHITE)
    for x in (xs[0], xs[-1]):
        y = wave_y(x)
        pen.ellipse([(x - half) * SCALE, (y - half) * SCALE, (x + half) * SCALE, (y + half) * SCALE],
                    fill=WHITE)

    return image.resize((SIZE, SIZE), Image.LANCZOS)


def main():
    icon = draw()
    out_dir = os.path.join(ROOT, "Data")
    png = os.path.join(out_dir, "Orca.png")
    icon.save(png)

    # macOS: an .iconset of the standard sizes, packed by iconutil.
    with tempfile.TemporaryDirectory() as tmp:
        iconset = os.path.join(tmp, "Orca.iconset")
        os.makedirs(iconset)
        for size in (16, 32, 128, 256, 512):
            icon.resize((size, size), Image.LANCZOS).save(
                os.path.join(iconset, f"icon_{size}x{size}.png"))
            icon.resize((size * 2, size * 2), Image.LANCZOS).save(
                os.path.join(iconset, f"icon_{size}x{size}@2x.png"))
        if shutil.which("iconutil"):
            subprocess.run(["iconutil", "-c", "icns", iconset, "-o",
                            os.path.join(out_dir, "Orca.icns")], check=True)
        else:
            print("orca-icon: no iconutil (macOS only): Data/Orca.icns was not updated and is now "
                  "out of step with Orca.png and Orca.ico", file=sys.stderr)

    # Windows: a multi-size .ico for the executable's resources.
    icon.save(os.path.join(ROOT, "Installer", "Orca.ico"),
              sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])


if __name__ == "__main__":
    main()
