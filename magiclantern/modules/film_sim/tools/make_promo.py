#!/usr/bin/env python3
"""Rebuild the two README images: one hero frame, and one frame in three stocks.

The old promo was a 3x2 contact sheet of six 65:24 frames. It was accurate but it
could not do a promo's job: at the width a README renders (about 1000 px) each
frame arrived at 490x181 and its caption at 3 px tall, so the stock names, the
ISO and the `.fs` filename -- the whole reason the sheet exists -- were
unreadable, and six different scenes cannot show what a *stock* does.

So it is two images instead:

  docs/img/promo.jpg   one frame, one stock, no type. The 3-second argument.
  docs/img/looks.jpg   the same frame through three stocks, labelled large
                       enough to read at that same 1000 px.

Both are raw develops from the camera's own files, at scale 2 (the level-3 path
this module runs), cropped to the 65:24 window Canon burned into its own JPEG of
the same frame -- so the aspect is the camera's framing, not a crop chosen here.

    python3 magiclantern/modules/film_sim/tools/make_promo.py [card_dir]

Needs the host harness built (`make -C magiclantern/modules/film_sim/se/hosttest`)
and the raw files on the card; nothing here downloads anything.
"""
import os
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
RAWDEV = os.path.abspath(os.path.join(HERE, "..", "se", "hosttest", "rawdev"))
FILMS = os.path.abspath(os.path.join(HERE, "..", "films"))
OUT = os.path.abspath(os.path.join(HERE, "..", "..", "..", "..", "docs", "img"))

# Hero: the horse in the pine grove. It has a subject, depth and a light source,
# which is what makes a thumbnail readable; Portra is the warmest of the three
# renderings tried and the one where the animal still reads at 500 px.
HERO = ("IMG_9919", "portra", "Portra 400")
# Comparison: two people on a road, so there is sky, foliage, tarmac and people
# in one frame -- three stocks separate visibly here.
COMPARE = ("IMG_9932", [("classicchrome", "Classic Chrome"),
                        ("portra", "Portra 400"),
                        ("velvia", "Velvia 50")])

HERO_W = 1600
CELL_W = 500
LABEL_H = 34          # at 1600 wide this lands at ~21 px on a 1000 px README
GAP = 12
BG = (12, 12, 12)
INK = (214, 214, 214)

FONT_CANDIDATES = [
    "/System/Library/Fonts/SFNSMono.ttf",
    "/System/Library/Fonts/Menlo.ttc",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
]


def font(size):
    for path in FONT_CANDIDATES:
        if os.path.exists(path):
            try:
                return ImageFont.truetype(path, size)
            except OSError:
                continue
    return ImageFont.load_default()


def window(jpg):
    """The rows Canon left uncovered, read off the JPEG it wrote for the same
    frame. That is the 65:24 window as the camera framed it."""
    im = Image.open(jpg).convert("RGB")
    w, h = im.size
    px = im.load()

    def dark(y):
        return max(sum(px[x, y]) for x in range(0, w, max(1, w // 48))) < 14

    top = 0
    while top < h and dark(top):
        top += 1
    bot = 0
    while bot < h and dark(h - 1 - bot):
        bot += 1
    if top == 0 and bot == 0:
        sys.exit("no letterbox bars in %s: was it shot at 65:24?" % jpg)
    return top, h - bot


def develop(card, frame, stock, out):
    """One level-3 develop on this machine, at scale 2."""
    r = subprocess.run([RAWDEV, os.path.join(card, frame + ".CR2"),
                        os.path.join(FILMS, stock + ".fs"), out, "2"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("rawdev %s + %s failed:\n%s%s" % (frame, stock, r.stdout, r.stderr))


def framed(card, frame, stock, width):
    """Develop, crop to the camera's 65:24 window, resize to `width`."""
    tmp = "/tmp/se-promo-%s-%s.jpg" % (frame, stock)
    develop(card, frame, stock, tmp)
    y0, y1 = window(os.path.join(card, frame + ".JPG"))
    im = Image.open(tmp).convert("RGB")
    jh = Image.open(os.path.join(card, frame + ".JPG")).size[1]
    if im.height != jh:
        sys.exit("%s: develop is %dx%d, the JPEG is %d rows"
                 % (frame, im.width, im.height, jh))
    im = im.crop((0, y0, im.width, y1))
    return im.resize((width, round(im.height * width / im.width)), Image.LANCZOS)


def main():
    card = sys.argv[1] if len(sys.argv) > 1 else "/Volumes/EOS_DIGITAL/DCIM/100CANON"
    os.makedirs(OUT, exist_ok=True)

    hero = framed(card, HERO[0], HERO[1], HERO_W)
    hero_path = os.path.join(OUT, "promo.jpg")
    hero.save(hero_path, quality=90, subsampling=0, optimize=True)
    print("promo.jpg  %-9s %-12s %dx%d  %4.0f KB"
          % (HERO[0], HERO[1], hero.width, hero.height,
             os.path.getsize(hero_path) / 1024))

    cells = [framed(card, COMPARE[0], s, CELL_W) for s, _ in COMPARE[1]]
    cw, ch = cells[0].size
    W = len(cells) * cw + (len(cells) - 1) * GAP
    H = LABEL_H + ch
    sheet = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(sheet)
    f = font(24)
    for i, (im, (_, label)) in enumerate(zip(cells, COMPARE[1])):
        x = i * (cw + GAP)
        sheet.paste(im, (x, LABEL_H))
        # 6 px inset, or the first label sits flush against the edge
        d.text((x + 6, 5), label.upper(), font=f, fill=INK)
    if cells[0].height != ch:
        sys.exit("cells differ in height")
    looks_path = os.path.join(OUT, "looks.jpg")
    sheet.save(looks_path, quality=90, subsampling=0, optimize=True)
    print("looks.jpg  %-9s %dx%d  %4.0f KB"
          % (COMPARE[0], sheet.width, sheet.height,
             os.path.getsize(looks_path) / 1024))


if __name__ == "__main__":
    main()
