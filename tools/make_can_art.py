#!/usr/bin/env python3
"""Draw 35 mm canister artwork for the stocks FILMCAN.DAT does not cover.

FILMCAN.DAT -- the artwork that ships with the FilmRoll module -- has fifteen
emulsions. The card carries more, and the shelf fell back to a procedural
drawing for the rest, which did not read as a film at all.

This draws the missing ones: a can, a printed wrap, the brand, the stock name,
the speed. Nothing is traced from a manufacturer's packaging; the shapes and the
lettering are produced here, which is why this file can live in the repository
while FILMCAN.DAT cannot.

Records are written in FILMCAN.DAT's own format so tools/merge_cans.py can join
them onto the user's copy:

    ML/FILMSIM/CANS.DAT  ("FCAN", shared palette, 170x240 images + 40x56 thumbs)

    python3 tools/make_can_art.py --base "<card>/ML/data/FILMCAN.DAT" --out /tmp/cans
"""
import argparse
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

W, H = 170, 240          # record geometry, same as FILMCAN.DAT
TW, TH = 40, 56          # thumbnail geometry
TRANSPARENT = 255        # index outside the palette: the drawer skips it

FONT_BOLD = "/System/Library/Fonts/Supplemental/Arial Bold.ttf"
FONT = "/System/Library/Fonts/Supplemental/Arial.ttf"

# The stocks FILMCAN.DAT has no record for, in card order.
#   name, brand, type block, speed, wrap colour, ink, side strip
MISSING = [
    ("CineStill 50D",   "CineStill", "50D",    "50",    "DAYLIGHT",       (32, 96, 150),   (255, 255, 255), (18, 56, 96)),
    ("Color 800",       "Kodak",     "Color",  "800",   "COLOUR NEG",     (44, 44, 52),    (250, 250, 250), (24, 24, 30)),
    ("C200",            "Kodak",     "C200",   "200",   "COLOUR NEG",     (34, 92, 74),    (250, 250, 250), (18, 54, 44)),
    ("Provia 100F",     "FUJIFILM",  "Provia", "100F",  "SLIDE",          (26, 60, 132),   (255, 255, 255), (14, 36, 84)),
    ("T-Max 100",       "Kodak",     "T-MAX",  "100",   "BLACK & WHITE",  (166, 166, 166), (26, 26, 28),    (104, 104, 104)),
    ("T-Max P3200",     "Kodak",     "T-MAX",  "P3200", "BLACK & WHITE",  (58, 58, 62),    (245, 245, 245), (34, 34, 38)),
    ("Delta 100",       "ILFORD",    "DELTA",  "100",   "BLACK & WHITE",  (238, 238, 238), (28, 28, 30),    (188, 188, 188)),
    ("Pan F Plus 50",   "ILFORD",    "PAN F",  "50",    "BLACK & WHITE",  (18, 18, 20),    (240, 240, 240), (10, 10, 12)),
    ("Acros 100",       "FUJIFILM",  "ACROS",  "100",   "BLACK & WHITE",  (30, 30, 34),    (235, 235, 235), (16, 16, 20)),
    ("Fomapan 100",     "FOMA",      "FOMAPAN", "100",  "BLACK & WHITE",  (196, 56, 46),   (250, 250, 250), (136, 32, 26)),
    ("XP2 Super",       "ILFORD",    "XP2",    "400",   "BLACK & WHITE",  (240, 236, 226), (26, 26, 28),    (184, 180, 170)),
]


def font(path, size):
    try:
        return ImageFont.truetype(path, size)
    except Exception:
        return ImageFont.load_default()


def fit(d, text, max_w, path, start, floor=11):
    """Largest size at or below `start` whose width fits `max_w`."""
    size = start
    while size > floor:
        f = font(path, size)
        if d.textlength(text, font=f) <= max_w:
            return f
        size -= 1
    return font(path, floor)


def shade(rgb, k):
    """k > 0 lightens towards white, k < 0 darkens towards black."""
    if k >= 0:
        return tuple(min(255, int(c + (255 - c) * k)) for c in rgb)
    return tuple(max(0, int(c * (1 + k))) for c in rgb)


def draw_canister(brand, code, iso, kind, band, ink, side):
    """Spool cap, metal body, printed wrap, side strip, bottom rim."""
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)

    x0, x1 = 22, 148
    y0, y1 = 26, 216

    d.rounded_rectangle([x0 + 10, 6, x1 - 10, y0 + 2], radius=5,
                        fill=(34, 34, 38, 255))
    d.rectangle([x0 + 34, 2, x1 - 34, 12], fill=(48, 48, 54, 255))
    d.rectangle([x0 + 34, 2, x1 - 34, 4], fill=(72, 72, 80, 255))

    for y in range(y0, y1):
        t = (y - y0) / float(y1 - y0)
        base = 46 + int(24 * max(0.0, 1.0 - abs(t - 0.35) * 2))
        d.line([(x0, y), (x1, y)], fill=(base, base, base + 2, 255))
    for gx, k in ((x0, -0.35), (x0 + 6, 0.55), (x1 - 6, 0.20), (x1, -0.45)):
        d.line([(gx, y0), (gx, y1)], fill=shade((46, 46, 48), k) + (255,))

    lx0, lx1 = x0 + 20, x1 - 8
    d.rectangle([lx0, y0 + 8, lx1, y1 - 26], fill=band + (255,))
    d.line([(lx0, y0 + 8), (lx1, y0 + 8)], fill=shade(band, 0.35) + (255,))
    d.line([(lx0, y1 - 26), (lx1, y1 - 26)], fill=shade(band, -0.45) + (255,))
    for i in range(9):
        d.line([(lx0 + 2 + i, y0 + 10), (lx0 + 2 + i, y1 - 28)],
               fill=shade(band, 0.12 - i * 0.03) + (255,))

    # Everything printed on the wrap is fitted to it: a clipped product name
    # reads as a bug, not as a design.
    wrap_w = (lx1 - lx0) - 12
    cx = (lx0 + lx1) // 2
    y = y0 + 14
    d.text((cx, y), brand, font=fit(d, brand, wrap_w, FONT_BOLD, 21),
           fill=ink + (255,), anchor="ma")
    y += 26

    words = code.split()
    f_code = fit(d, code if len(words) == 1 else max(words, key=len),
                 wrap_w, FONT_BOLD, 34)
    if len(words) > 1:
        for wd in words:
            d.text((cx, y), wd, font=f_code, fill=ink + (255,), anchor="ma")
            y += int(f_code.size * 0.95)
    else:
        d.text((cx, y + 4), code, font=f_code, fill=ink + (255,), anchor="ma")
        y += int(f_code.size * 1.25)

    f_iso = fit(d, iso, wrap_w, FONT_BOLD, 40)
    d.text((cx, y), iso, font=f_iso, fill=ink + (255,), anchor="ma")
    y += int(f_iso.size * 1.15)
    d.text((cx, y), "35 mm", font=fit(d, "35 mm", wrap_w, FONT, 15),
           fill=shade(ink, -0.15) + (255,), anchor="ma")
    y += 17
    d.text((cx, y), "36 EXP.", font=fit(d, "36 EXP.", wrap_w, FONT_BOLD, 15),
           fill=ink + (255,), anchor="ma")

    # Side strip: colour only. Words there were 13 px wide and always clipped,
    # and the wrapper already says what kind of film it is.
    sx0, sx1 = x0 + 4, x0 + 17
    d.rectangle([sx0, y0 + 8, sx1, y1 - 26], fill=side + (255,))
    d.line([(sx0 + 5, y0 + 12), (sx0 + 5, y1 - 30)], fill=shade(side, 0.5) + (255,))

    d.rectangle([x0, y1 - 20, x1, y1], fill=(30, 30, 34, 255))
    d.line([(x0, y1 - 20), (x1, y1 - 20)], fill=(66, 66, 72, 255))
    d.ellipse([x0 + 30, y1 - 14, x1 - 30, y1 - 2], fill=(20, 20, 24, 255))
    return im


def to_indexed(im, pal_img, ncolors):
    """RGB -> indices into the shared palette; fully transparent -> 255."""
    flat_rgb = Image.new("RGB", im.size, (6, 6, 6))
    flat_rgb.paste(im.convert("RGB"), (0, 0), im.split()[3])
    q = flat_rgb.quantize(palette=pal_img, dither=Image.FLOYDSTEINBERG)
    idx = bytearray(q.tobytes())
    alpha = im.split()[3].tobytes()
    for i, a in enumerate(alpha):
        if a < 40:
            idx[i] = TRANSPARENT
    return idx


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True,
                    help="the user's FILMCAN.DAT, for its palette")
    ap.add_argument("--out", required=True, help="directory for the records")
    args = ap.parse_args()

    raw = open(args.base, "rb").read()
    count, ncolors, w, h, tw, th = struct.unpack("<6H", raw[4:16])
    pal = raw[16:16 + ncolors * 3]
    print("base: %d records, %d colours, %dx%d" % (count, ncolors, w, h))
    if (w, h, tw, th) != (W, H, TW, TH):
        sys.exit("geometry %dx%d/%dx%d does not match this script" % (w, h, tw, th))

    flat = list(pal) + [0, 0, 0] * (256 - ncolors)
    pal_img = Image.new("P", (1, 1))
    pal_img.putpalette(flat)

    os.makedirs(args.out, exist_ok=True)
    for name, brand, code, iso, kind, band, ink, side in MISSING:
        art = draw_canister(brand, code, iso, kind, band, ink, side)
        idx = to_indexed(art, pal_img, ncolors)

        thumb = art.resize((TW, TH), Image.LANCZOS)
        tidx = to_indexed(thumb, pal_img, ncolors)

        safe = name.replace(" ", "_").replace("-", "").replace(".", "")
        open(os.path.join(args.out, safe + ".IMG"), "wb").write(bytes(idx))
        open(os.path.join(args.out, safe + ".THM"), "wb").write(bytes(tidx))
        thumb.save(os.path.join(args.out, safe + ".PNG"))
        print("  %-16s %s" % (name, safe))


if __name__ == "__main__":
    sys.exit(main())
