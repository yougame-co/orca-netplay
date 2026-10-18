#!/usr/bin/env python3
# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
"""Orca's Online menu labels: original art, drawn in each game's style with open-licence fonts.

Writes the texture pack Orca loads in every session (Data/Sys/Orca/Textures/RSBE01; see ORCA.md
"Online menu" and "On-screen UI"). Each PNG uses a Dolphin hires texture name, so it replaces
one game texture on screen and never touches emulated memory. Brawl and Project+ both load
textures as RSBE01, but their labels hash differently, so one folder serves both.

  python3 Tools/orca/textures/make-labels.py [--out <dir>] [--sheet <preview.png>]

Needs Pillow. The fonts sit next to this script with their SIL Open Font License 1.1 texts: Archivo
Black (Brawl's labels), Anton (Project+'s labels, Brawl's title bar, STAGE SELECT, Project+'s
legend), Russo One (READY TO FIGHT!) and Cinzel (Brawl's stage select title). Nothing is derived
from the game's art; the boxes below only record where the game's letters sit.

Labels are drawn at 4x with mip levels down to 1x (_mip1.png at 2x, _mip2.png at 1x). Dolphin
samples a custom texture's mips even when the game's texture has none, so at 1x internal resolution
thin strokes stay clean.
"""

import argparse
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
FONTS = os.path.join(HERE, "fonts")
ARCHIVO = os.path.join(FONTS, "ArchivoBlack-Regular.ttf")
ANTON = os.path.join(FONTS, "Anton-Regular.ttf")
RUSSO = os.path.join(FONTS, "RussoOne-Regular.ttf")
CINZEL = os.path.join(FONTS, "Cinzel[wght].ttf")
SCALE = 4

# One entry per replaced texture:
#   name: Dolphin's name for the game's texture (tex1_<w>x<h>_<XXH64 of its data>_<GX format>)
#   lines: the text, one string per line
#   box: where the game's own letters sit in its texture (x0, y0, x1, y1), native pixels
#   style: brawl (black, white rim and soft halo), pplus (black, white outline), title (white),
#          brawl-css and pplus-css (the character select's mode title: a gradient fill, black outline)
LABELS = [
    # Brawl's main menu: the Wi-Fi button's label (MenMainFrTop15, "NINTENDO Wi-Fi CONNECTION").
    dict(name="tex1_127x96_fdd5334e5af7357c_2", lines=["Play", "Online"], box=(7, 12, 119, 84),
         style="brawl"),
    # Brawl's page title and breadcrumb on the Online pages (MenMainTtlJ.03, "NINTENDO WFC"): its
    # letters span the slot at the height of every title's (rows 1-14); ours take that height.
    dict(name="tex1_120x16_a56825159146a723_0", lines=["ONLINE"], box=(3, 1, 117, 14),
         style="title"),
    # Brawl's With Anyone: Basic Brawl (MenMainWifi22) and Team Battle (MenMainWifi23).
    dict(name="tex1_144x88_115d82941c7412cd_2", lines=["Casual"], box=(7, 22, 138, 66),
         style="brawl"),
    # (The game's letters reach x 151; ours stop at 140, so RANKED's rim clears the handshake icon
    # as CASUAL's clears its own.)
    dict(name="tex1_160x80_e364b4db513567ba_2", lines=["Ranked"], box=(7, 18, 140, 62),
         style="brawl"),
    # Project+'s With Anyone: Basic Versus (MenMainWifi22) and Team Battle (MenMainWifi23).
    dict(name="tex1_144x88_ecd09d11be34045f_5", lines=["CASUAL"], box=(11, 16, 133, 71),
         style="pplus"),
    dict(name="tex1_160x80_5b8f52ac0127b2a8_5", lines=["RANKED"], box=(24, 14, 136, 66),
         style="pplus"),
    # The character select's mode title (MenSelchrPanelRule3.1), one per mode. Orca marks the mode
    # in an invisible palette entry (UX/CssTitle.h), so each mode hashes to its own name. Brawl's
    # uses the gold of its other rule titles; Project+'s the mint of its VERSUS.
    dict(name="tex1_132x48_44a1dd712ec999f0_159b390ed5eec04d_9", lines=["Casual"],
         box=(8, 12, 124, 36), style="brawl-css"),
    dict(name="tex1_132x48_44a1dd712ec999f0_4d236969243ec2d8_9", lines=["Ranked"],
         box=(8, 12, 124, 36), style="brawl-css"),
    dict(name="tex1_132x48_44a1dd712ec999f0_952be74b413d03a4_9", lines=["Friends"],
         box=(8, 12, 124, 36), style="brawl-css"),
    dict(name="tex1_128x56_57564f7b1ba3bc1c_96a987e9e0ec657e_9", lines=["CASUAL"],
         box=(16, 17, 113, 39), style="pplus-css"),
    dict(name="tex1_128x56_57564f7b1ba3bc1c_e2dffeab0d95d424_9", lines=["RANKED"],
         box=(16, 17, 113, 39), style="pplus-css"),
    dict(name="tex1_128x56_57564f7b1ba3bc1c_faaf2c3553d21273_9", lines=["FRIENDS"],
         box=(13, 17, 116, 39), style="pplus-css"),
]

# The relabels (Core/Orca/Games/Brawl/Relabel.h, ORCA.md "On-screen UI"): the game's big words say what
# to do now. Orca marks each texture with an invisible value, so each label hashes to its own name:
#   band-<game>-<n>   READY TO FIGHT! (MenSelchrReady01_2, C4 352x36): 1 PRESS START TO LOCK IN!,
#                     2 WINNER PICKS FIRST!, 3 LOSER PICKS NOW!
#   line-<game>-<n>   STAGE SELECT (MenSelmapBack07, I4; Brawl 128x16, Project+ 189x30): whose
#                     turn and what, Relabel::LineFor's numbering
#   title-<game>-<n>  the stage select's BRAWL / VERSUS (MenSelchrRtitle.30, I4 120x24): 1 RANKED,
#                     2 CASUAL
#   legend-info       Project+'s stage select legend's words and icons (CornerInfo, I4 245x53)
#   legend-buttons    its buttons (CornerButtons, IA8 150x50): X, A, B
# Names are Dolphin's hashes of the marked textures.
RELABELS = [
    ("band-brawl-1", "tex1_352x36_260eae27613f7b8a_0f0d1db67c43a0e3_8", "PRESS START TO LOCK IN!"),
    ("band-pplus-1", "tex1_352x36_ca0167082a3a208f_d479558876e2417e_8", "PRESS START TO LOCK IN!"),
    ("band-brawl-2", "tex1_352x36_260eae27613f7b8a_666aa1e6e564359f_8", "WINNER PICKS FIRST!"),
    ("band-pplus-2", "tex1_352x36_ca0167082a3a208f_93e0d76a7dbd4ba2_8", "WINNER PICKS FIRST!"),
    ("band-brawl-3", "tex1_352x36_260eae27613f7b8a_b25d489bffb02296_8", "LOSER PICKS NOW!"),
    ("band-pplus-3", "tex1_352x36_ca0167082a3a208f_dbed1c4865ad6bc6_8", "LOSER PICKS NOW!"),
    ("line-brawl-1", "tex1_128x16_79a80b5f41fb3faf_0", "P1 STRIKES 1"),
    ("line-pplus-1", "tex1_189x30_5ff5c75022995de6_0", "P1 STRIKES 1"),
    ("line-brawl-2", "tex1_128x16_4d669b592545413a_0", "P1 STRIKES 2"),
    ("line-pplus-2", "tex1_189x30_d1303babebb83c7a_0", "P1 STRIKES 2"),
    ("line-brawl-3", "tex1_128x16_76c379d19487a91b_0", "P1 STRIKES 3"),
    ("line-pplus-3", "tex1_189x30_e81fc524332deea9_0", "P1 STRIKES 3"),
    ("line-brawl-4", "tex1_128x16_8f915d2039abe0b3_0", "P2 STRIKES 1"),
    ("line-pplus-4", "tex1_189x30_028f53f3362a11c8_0", "P2 STRIKES 1"),
    ("line-brawl-5", "tex1_128x16_29d872a8b2573932_0", "P2 STRIKES 2"),
    ("line-pplus-5", "tex1_189x30_4b347d5515f6b6b6_0", "P2 STRIKES 2"),
    ("line-brawl-6", "tex1_128x16_08fc7a7f0e79bc3a_0", "P2 STRIKES 3"),
    ("line-pplus-6", "tex1_189x30_876c88de030de85a_0", "P2 STRIKES 3"),
    ("line-brawl-7", "tex1_128x16_d109656f148b9555_0", "P1 BANS 1"),
    ("line-pplus-7", "tex1_189x30_5f998e28cdb67a31_0", "P1 BANS 1"),
    ("line-brawl-8", "tex1_128x16_7562692397982484_0", "P1 BANS 2"),
    ("line-pplus-8", "tex1_189x30_3a09724fd5016586_0", "P1 BANS 2"),
    ("line-brawl-9", "tex1_128x16_a2448cb4b7da1b3c_0", "P2 BANS 1"),
    ("line-pplus-9", "tex1_189x30_f0cfeb3c53e07be7_0", "P2 BANS 1"),
    ("line-brawl-10", "tex1_128x16_318db95cc34a3886_0", "P2 BANS 2"),
    ("line-pplus-10", "tex1_189x30_9702d00c0e686aa2_0", "P2 BANS 2"),
    ("line-brawl-11", "tex1_128x16_c5f131b811d425d3_0", "P1 PICKS"),
    ("line-pplus-11", "tex1_189x30_79f7db5ce7c05cef_0", "P1 PICKS"),
    ("line-brawl-12", "tex1_128x16_cf81f52a3b1d38d7_0", "P2 PICKS"),
    ("line-pplus-12", "tex1_189x30_0e65069734c8504f_0", "P2 PICKS"),
    ("line-brawl-13", "tex1_128x16_bd1aa1d7e611c8d2_0", "PICK A STAGE"),
    ("line-pplus-13", "tex1_189x30_392ed36467f522bc_0", "PICK A STAGE"),
    ("title-brawl-1", "tex1_120x24_8c349c96744f9bc7_0", "RANKED"),
    ("title-pplus-1", "tex1_120x24_1604ccdc8de0780d_0", "RANKED"),
    ("title-brawl-2", "tex1_120x24_8e68a6bb69e7335c_0", "CASUAL"),
    ("title-pplus-2", "tex1_120x24_0ec8c86664faeefc_0", "CASUAL"),
    ("legend-info", "tex1_245x53_c522515513a179c6_0", "STRIKE PROPOSE TAKE BACK"),
    ("legend-buttons", "tex1_150x50_fca24ef397f2d1e0_3", "X A B"),
]


def fitted(mask, box, w, h, align="centre", max_squash=0.62):
    """`mask` scaled to the box's height (native pixels, drawn at 4x) and squeezed sideways, to
    `max_squash` of its width at most, when too wide (then smaller); placed in the box."""
    W, H = w * SCALE, h * SCALE
    x0, y0, x1, y1 = (v * SCALE for v in box)
    bw, bh = x1 - x0, y1 - y0
    k = bh / mask.height
    kx = k
    if mask.width * kx > bw:
        kx = max(bw / mask.width, k * max_squash)
        if mask.width * kx > bw:
            k *= bw / (mask.width * kx)
            kx = bw / mask.width
    r = mask.resize((max(1, int(mask.width * kx)), max(1, int(mask.height * k))), Image.LANCZOS)
    out = Image.new("L", (W, H), 0)
    x = x0 + (bw - r.width) // 2 if align == "centre" else x1 - r.width
    out.paste(r, (x, y0 + (bh - r.height) // 2))
    return out


def font_mask(text, path, tracking, weight=None):
    """`text` in the font at 400 px as a tight L mask; `weight` a variable font's wght axis."""
    f = ImageFont.truetype(path, 400)
    if weight:
        f.set_variation_by_axes([weight])
    return line_mask([(text, f)], 400 * tracking)


def slanted(mask, slant):
    """`mask` in italics: each row moved right by `slant` times its height above the bottom."""
    w, h = mask.size
    extra = int(round(slant * h))
    return mask.transform((w + extra, h), Image.AFFINE, (1, slant, -extra, 0, 1, 0),
                          resample=Image.BICUBIC)


def intensity(mask):
    """An I4 texture's art: the game draws intensity as both colour and alpha."""
    return Image.merge("RGBA", (mask, mask, mask, mask))


def draw_relabel(key, name, text):
    w, h = size_of(name)
    W, H = w * SCALE, h * SCALE
    kind, game = key.split("-")[0], (key.split("-")[1] if key.count("-") == 2 else "")
    if kind == "band":
        # White letters in a dark outline, which the game tints, like its own READY TO FIGHT!.
        m = fitted(font_mask(text, RUSSO, 0.02), (4, 4, 348, 32), w, h)
        out = Image.new("RGBA", (W, H), (255, 255, 255, 0))
        out = Image.composite(Image.new("RGBA", (W, H), (33, 33, 33, 255)), out,
                              grow(m, 2.6 * SCALE))
        return Image.composite(Image.new("RGBA", (W, H), (255, 255, 255, 255)), out, m)
    if kind == "line":
        # In italics like the game's STAGE SELECT, right-aligned: the overlay's timer follows the
        # texture's right end (Overlay.cpp).
        box = (1, 1, 127, 15) if game == "brawl" else (1, 2, 188, 28)
        return intensity(fitted(slanted(font_mask(text, ANTON, 0.03), 0.22), box, w, h,
                                align="right"))
    if kind == "title":
        # Brawl's BRAWL is a serif in capitals; Project+'s VERSUS a condensed sans.
        if game == "brawl":
            m = fitted(font_mask(text, CINZEL, 0.0, weight=900), (17, 3, 103, 21), w, h)
        else:
            m = fitted(font_mask(text, ANTON, 0.03), (2, 3, 68, 22), w, h)
        return intensity(m)
    if key == "legend-info":
        m = Image.new("L", (W, H), 0)
        for word, box in (("STRIKE", (1, 1, 67, 21)), ("PROPOSE", (84, 1, 150, 21)),
                          ("TAKE BACK", (166, 1, 244, 21))):
            m = ImageChops.lighter(m, fitted(font_mask(word, ANTON, 0.03), box, w, h))
        d = ImageDraw.Draw(m)
        s, lw = SCALE, int(4.2 * SCALE)
        # Under each word its sign: a cross, a tick, an arrow curling back.
        d.line([(8 * s, 27 * s), (33 * s, 48 * s)], fill=255, width=lw)
        d.line([(33 * s, 27 * s), (8 * s, 48 * s)], fill=255, width=lw)
        d.line([(88 * s, 38 * s), (96 * s, 47 * s), (110 * s, 27 * s)], fill=255, width=lw,
               joint="curve")
        d.arc([(170 * s, 27 * s), (196 * s, 50 * s)], 200, 450, fill=255, width=lw)
        d.polygon([(166 * s, 31 * s), (178 * s, 26 * s), (176 * s, 39 * s)], fill=255)
        return intensity(m)
    # legend-buttons: the GameCube's X, A and B where the game's own buttons sit.
    s = SCALE
    out = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(out)
    f = ImageFont.truetype(ANTON, int(26 * s))
    for letter, cx, r in (("X", 17.5, 15.5), ("A", 61.5, 23), ("B", 119, 20)):
        c = (cx * s, 25 * s)
        d.ellipse([c[0] - r * s, c[1] - r * s, c[0] + r * s, c[1] + r * s], fill=(60, 60, 60, 255))
        rr = r - 2.2
        d.ellipse([c[0] - rr * s, c[1] - rr * s, c[0] + rr * s, c[1] + rr * s],
                  fill=(222, 222, 222, 255))
        d.text(c, letter, font=f, fill=(56, 56, 56, 255), anchor="mm")
    return out


# The character select titles' fills, top to bottom (position 0-1 down the letters, RGB): measured
# on the game's own rule titles, a gold with a white band through the middle (Brawl's) and a teal
# to white to green (Project+'s VERSUS).
GRADIENTS = {
    "brawl-css": [(0.0, (221, 204, 102)), (0.3, (221, 204, 110)), (0.42, (238, 221, 170)),
                  (0.5, (255, 246, 220)), (0.56, (255, 255, 255)), (0.62, (255, 246, 220)),
                  (0.7, (238, 221, 170)), (0.8, (221, 204, 112)), (1.0, (221, 204, 102))],
    "pplus-css": [(0.0, (65, 222, 189)), (0.25, (148, 230, 205)), (0.45, (222, 246, 230)),
                  (0.55, (222, 246, 230)), (0.75, (139, 230, 172)), (1.0, (74, 213, 123))],
}


def size_of(name):
    w, h = name.split("_")[1].split("x")
    return int(w), int(h)


def gradient(style, top, bottom, w, h):
    """An RGBA image of the style's fill, its stops spread from row `top` to row `bottom`."""
    stops = GRADIENTS[style]
    out = Image.new("RGBA", (w, h))
    px = out.load()
    for y in range(h):
        t = min(1.0, max(0.0, (y - top) / max(1, bottom - top)))
        for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
            if t0 <= t <= t1:
                k = (t - t0) / (t1 - t0) if t1 > t0 else 0
                c = tuple(round(a + (b - a) * k) for a, b in zip(c0, c1))
                break
        for x in range(w):
            px[x, y] = c + (255,)
    return out


def small_caps_runs(word, font_path, size):
    """Brawl's label lettering: each word's first letter full size, the rest small capitals."""
    runs = []
    for i, word_part in enumerate(word.split(" ")):
        if i:
            runs.append((" ", ImageFont.truetype(font_path, int(size * 0.62))))
        runs.append((word_part[0].upper(), ImageFont.truetype(font_path, size)))
        if len(word_part) > 1:
            runs.append((word_part[1:].upper(), ImageFont.truetype(font_path, int(size * 0.74))))
    return runs


def line_mask(runs, tracking):
    """One line of text (runs of (text, font) sharing a baseline) as a tight L mask."""
    ascent = max(f.getmetrics()[0] for _, f in runs)
    descent = max(f.getmetrics()[1] for _, f in runs)
    width = 0
    for text, f in runs:
        width += sum(f.getlength(c) + tracking for c in text)
    mask = Image.new("L", (int(width) + 8, ascent + descent + 8), 0)
    d = ImageDraw.Draw(mask)
    x = 4.0
    for text, f in runs:
        a = f.getmetrics()[0]
        for c in text:
            d.text((x, 4 + ascent - a), c, font=f, fill=255)
            x += f.getlength(c) + tracking
    return mask.crop(mask.getbbox())


def text_mask(label, w, h):
    """The label's letters as an L mask of the full texture (4x), fitted into its box."""
    W, H = w * SCALE, h * SCALE
    x0, y0, x1, y1 = (v * SCALE for v in label["box"])
    bw, bh = x1 - x0, y1 - y0
    style = label["style"]
    big = 400
    lines = []
    for text in label["lines"]:
        if style in ("brawl", "brawl-css"):
            lines.append(line_mask(small_caps_runs(text, ARCHIVO, big), big * 0.01))
        elif style in ("pplus", "pplus-css"):
            lines.append(line_mask([(text, ImageFont.truetype(ANTON, big))], big * 0.02))
        else:
            # Anton as it comes (condensed, like the game's titles), with hardly any tracking.
            lines.append(line_mask([(text, ImageFont.truetype(ANTON, big))], big * 0.03))
    gap = int(big * 0.14)
    tw = max(m.width for m in lines)
    th = sum(m.height for m in lines) + gap * (len(lines) - 1)
    block = Image.new("L", (tw, th), 0)
    y = 0
    for m in lines:
        block.paste(m, ((tw - m.width) // 2, y))
        y += m.height + gap
    # The title bar's letters fill its height (centred, as the game's own short titles are); a label
    # keeps its aspect inside its box.
    k = min(bw / tw, bh / th)
    if style == "title":
        k = bh / th
    kx = k
    if style == "pplus-css":
        # Project+'s title letters are wider than Anton's: its full height, widened to the box.
        k = bh / th
        kx = min(bw / tw, k * 1.6)
    block = block.resize((max(1, int(tw * kx)), max(1, int(th * k))), Image.LANCZOS)
    out = Image.new("L", (W, H), 0)
    out.paste(block, (x0 + (bw - block.width) // 2, y0 + (bh - block.height) // 2))
    return out


def grow(mask, px):
    """The mask dilated by about `px` pixels, with round corners and a soft edge: a blur whose
    sigma is px / 2 reaches 2.3% of full (6/255) two sigmas out."""
    if px <= 0:
        return mask
    return mask.filter(ImageFilter.GaussianBlur(px / 2)).point(
        lambda v: 255 if v >= 6 else v * 42)


def draw(label):
    w, h = size_of(label["name"])
    W, H = w * SCALE, h * SCALE
    mask = text_mask(label, w, h)
    style = label["style"]
    if style == "title":
        # An I4 texture: the game draws intensity as both colour and alpha.
        return Image.merge("RGBA", (mask, mask, mask, mask))
    white = Image.new("RGBA", (W, H), (255, 255, 255, 255))
    black = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    out = Image.new("RGBA", (W, H), (255, 255, 255, 0))
    if style == "brawl":
        rim = grow(mask, 2.2 * SCALE)
        halo = grow(rim, 2.5 * SCALE).filter(ImageFilter.GaussianBlur(1.8 * SCALE))
        out = Image.composite(white, out, halo.point(lambda v: int(v * 0.6)))
        out = Image.composite(white, out, rim)
    elif style in GRADIENTS:
        # The letters in the style's fill, its stops down the letters' own height, inside a black
        # outline about two and a half of the game's pixels wide, as the game's titles have.
        box = mask.point(lambda v: 255 if v >= 128 else 0).getbbox()
        fill = gradient(style, box[1], box[3], W, H)
        outline = grow(mask, 2.4 * SCALE)
        out = Image.composite(black, out, outline)
        return Image.composite(fill, out, mask)
    else:
        rim = grow(mask, 2.6 * SCALE)
        edge = grow(rim, 0.8 * SCALE)
        out = Image.composite(Image.new("RGBA", (W, H), (60, 60, 60, 255)), out,
                              ImageChops.subtract(edge, rim).point(lambda v: int(v * 0.5)))
        out = Image.composite(white, out, rim)
    return Image.composite(black, out, mask)


def mips(label, im):
    """The label's mip levels, each half the last (box filter), down to the game's size. I4
    textures (intensity is both colour and alpha) halve their mask instead of the RGBA image."""
    assert SCALE & (SCALE - 1) == 0, "mip levels halve down to the game's size: SCALE is 2^n"
    w, h = size_of(label["name"])
    out = []
    while im.width > w:
        size = (im.width // 2, im.height // 2)
        if label["style"] in ("title", "intensity"):
            a = im.getchannel("A").resize(size, Image.BOX)
            im = Image.merge("RGBA", (a, a, a, a))
        else:
            im = im.resize(size, Image.BOX)
        out.append(im)
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--out", default=os.path.join(REPO, "Data", "Sys", "Orca", "Textures", "RSBE01"))
    p.add_argument("--sheet", help="also write a preview of every label on the menus' colours")
    a = p.parse_args()
    os.makedirs(a.out, exist_ok=True)
    images = []
    work = [(label, lambda label=label: draw(label)) for label in LABELS]
    for key, name, text in RELABELS:
        kind = key.split("-")[0]
        style = "intensity" if kind in ("line", "title") or key == "legend-info" else "relabel"
        work.append((dict(name=name, style=style),
                     lambda key=key, name=name, text=text: draw_relabel(key, name, text)))
    for label, make in work:
        im = make()
        path = os.path.join(a.out, label["name"] + ".png")
        im.save(path, optimize=True)
        images.append((label, im))
        print(path, im.size)
        for level, mip in enumerate(mips(label, im), 1):
            path = os.path.join(a.out, f"{label['name']}_mip{level}.png")
            mip.save(path, optimize=True)
            print(path, mip.size)
    if a.sheet:
        cw = max(im.width for _, im in images) + 20
        sheet = Image.new("RGBA", (cw * 2, sum(im.height + 20 for _, im in images)), (0, 0, 0, 255))
        y = 0
        for label, im in images:
            for i, bg in enumerate([(236, 168, 196, 255), (40, 30, 70, 255)]):
                tile = Image.new("RGBA", (im.width, im.height), bg)
                tile.alpha_composite(im)
                sheet.paste(tile, (cw * i + 10, y + 10))
            y += im.height + 20
        sheet.save(a.sheet)
    return 0


if __name__ == "__main__":
    sys.exit(main())
