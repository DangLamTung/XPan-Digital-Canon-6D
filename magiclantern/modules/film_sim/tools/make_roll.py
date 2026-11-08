#!/usr/bin/env python3
"""Build docs/img/roll.jpg: six frames, each with the stock that suits it.

The frames and their pairings were chosen by the camera's owner, not by a
score: a road under pines, two people in a sunlit grove, a rocky slope in
flare, a Circle K at night, a hazy lake, a lookout terrace at golden hour.

Each pairing is meant to be the stock that scene would actually have been shot
on:

  road, deep shade, cool                CLASSIC CHROME   documentary, muted
  two people, high key, backlit         PORTRA 400       forgiving, warm skin
  rock and pine, hard sun               VELVIA 50        saturated slide
  Circle K, tungsten and neon           SOUP 800         Vision3 500T, halation
  lake, hazy afternoon                  GOLD 200         consumer warm
  lookout, golden hour, backlit         PRO 400H         airy, pastel

Every cell is a level-3 raw develop from the card (rawdev, scale 2), framed at
65:24: cropped to the letterbox window Canon burned into its own JPEG when the
frame was shot that way, and centre-cropped when it was not (the night shot is
3:2 and its dark sky looks exactly like a letterbox bar, so a bar detector
cannot be trusted there).

    python3 magiclantern/modules/film_sim/tools/make_roll.py [card_dir]

Prints the colour of every cell -- mean RGB, HSV saturation, clipped and
crushed -- because "is the colour right" should be a number, not an opinion.
"""
import os
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
RAWDEV = os.path.abspath(os.path.join(HERE, "..", "se", "hosttest", "rawdev"))
FILMS = os.path.abspath(os.path.join(HERE, "..", "films"))
OUT = os.path.abspath(os.path.join(HERE, "..", "..", "..", "..", "docs", "img"))

CELL_W = 760
LABEL_H = 34
GAP = 12
COLS = 2
BG = (12, 12, 12)
INK = (216, 216, 216)

# No re-development here.
#
# These cells are the camera's own JPEGs, copied from the card and cropped to
# the 65:24 window the camera burned in -- not re-rendered on this machine. The
# raw-develop renders that were here before were the wrong thing to show: 9916
# needed 2.4 EV of lift to match Canon's render and came out over-processed,
# 9915 landed brighter than the file the camera wrote, and 9930/9914 were being
# replaced with a stock's idea of a scene instead of the photograph the camera
# actually produced. "Is the colour right" has an unambiguous answer when the
# pixels are the card's: they are the card's.
#
# A stock render is still one flag away -- give a frame a stock name instead of
# None below -- but the sheet as shipped shows what the camera made.
# (frame, stock or None for the card's JPEG, label, EV offset from the
# exposure that matches Canon's own render)
STOCKS = [
    ("IMG_9914", None, "IMG_9914  -  as shot on the card", 0.0),
    # 9915 is blown on the card -- the camera's own JPEG measures mean luma 180
    # with the haze sitting at 238, so this is the one cell that is a develop
    # rather than the file, pulled 1.2 EV under Canon's rendering to bring the
    # haze back into the print.
    ("IMG_9915", "portra", "IMG_9915  -  Portra 400, 1.2 EV under the card", -1.2),
    ("IMG_9930", None, "IMG_9930  -  as shot on the card", 0.0),
    ("IMG_0049", None, "IMG_0049  -  Circle K, as shot", 0.0),
    ("IMG_9937", None, "IMG_9937  -  as shot on the card", 0.0),
    ("IMG_9916", None, "IMG_9916  -  as shot on the card", 0.0),
]

FONTS = ["/System/Library/Fonts/SFNSMono.ttf",
         "/System/Library/Fonts/Menlo.ttc",
         "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"]


def font(size):
    for p in FONTS:
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, size)
            except OSError:
                continue
    return ImageFont.load_default()


def find(card, frame, ext):
    for sub in ("", "100CANON/", "101CANON/"):
        p = os.path.join(card, sub, frame + ext)
        if os.path.exists(p):
            return p
    sys.exit("not on the card: %s%s" % (frame, ext))


def window(jpg):
    """Letterbox window from the camera's own JPEG, or None if it has none."""
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
    # A night sky is black too: only call it a letterbox if the leftovers are
    # thick enough to be bars and the window is actually 65:24.
    if (top + bot) < h * 0.15:
        return None
    ratio = w / (h - top - bot)
    if not (2.55 < ratio < 2.85):
        return None
    return top, h - bot


# Why every recipe is re-exposed here.
#
# The shipped stocks carry no EXPOSURE line, and the repo's own measuring stick
# says what that costs: samples/base-neutral.fs, which is the identity look with
# nothing but exposure, records that at 0 the develop is ~1.3 stops darker than
# the camera's JPEG of the same frame, and 1.8 puts them within a third of a
# stop. Rendering a promo at 0 therefore shows a darker, flatter picture than the
# module actually produces in use -- and that is what the first attempt at this
# sheet did. Each cell is rendered with the shipped recipe plus EXPOSURE 1.8,
# which is the same stock at the exposure its own notes say it needs; nothing
# else in the recipe is touched.
EXPOSURE_EV = 1.8      # the upper probe of the exposure search
EV_MIN, EV_MAX = 0.0, 2.4


def recipe_with_exposure(stock, ev):
    """The shipped recipe, with its EXPOSURE set. Nothing else changes."""
    src = os.path.join(FILMS, stock + ".fs")
    out = "/tmp/se-roll-%s.fs" % stock
    with open(src) as f:
        lines = f.read().splitlines()
    kept = [l for l in lines if not l.strip().upper().startswith("EXPOSURE")]
    kept.insert(1, "EXPOSURE  %.2f" % ev)
    with open(out, "w") as f:
        f.write("\n".join(kept) + "\n")
    return out


def clipped(im):
    px = list(im.convert("L").getdata())
    return sum(1 for v in px if v >= 250) / len(px) * 100


def mean_luma(im):
    px = list(im.convert("L").getdata())
    return sum(px) / len(px)


def develop(cr2, stock, ev, tag):
    out = "/tmp/se-roll-%s-%s-%s.jpg" % (os.path.basename(cr2)[:-4], stock, tag)
    r = subprocess.run([RAWDEV, cr2, recipe_with_exposure(stock, ev), out, "2"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("rawdev %s + %s at EV %.1f failed:\n%s%s"
                 % (cr2, stock, ev, r.stdout, r.stderr))
    return Image.open(out).convert("RGB")


def framed(im, win):
    """65:24, as the module crops it: the letterbox window if the frame had
    one, otherwise a centred crop. Applied to the camera's reference too, so
    the two measurements are of the same rectangle."""
    if win:
        return im.crop((0, win[0], im.width, win[1]))
    want = round(im.width * 24 / 65)
    if want > im.height:
        sys.exit("too little height for 65:24")
    y0 = (im.height - want) // 2
    return im.crop((0, y0, im.width, y0 + want))


def cell(card, frame, stock, ev_delta=0.0):
    """The camera's own JPEG if no stock is given, else a raw develop; either
    way cropped to the 65:24 window the camera framed."""
    jpg = find(card, frame, ".JPG")
    win = window(jpg)
    if stock is None:
        im = framed(Image.open(jpg).convert("RGB"), win)
        ref = im
        ref_clip = clipped(ref)
        print("  %-10s card JPEG  luma %.0f  clip %.2f%%"
              % (frame, mean_luma(im), ref_clip))
        return im.resize((CELL_W, round(im.height * CELL_W / im.width)),
                         Image.LANCZOS)
    return _develop_cell(card, frame, stock, jpg, win, ev_delta)


def _develop_cell(card, frame, stock, jpg, win, ev_delta):
    """Render, expose to match Canon's own JPEG of the frame, crop to 65:24.

    Both are measured on the cropped rectangle. The first version compared the
    develop's full-frame mean against the JPEG's window-only mean, which is not
    the same picture: the develop has no bars, so it carried bright sky that the
    reference did not, and the exposure was fitted to the wrong number.
    """
    cr2 = find(card, frame, ".CR2")
    ref = framed(Image.open(jpg).convert("RGB"), win)
    target, ref_clip = mean_luma(ref), clipped(ref)

    lo = framed(develop(cr2, stock, 0.0, "ev0"), win)
    hi = framed(develop(cr2, stock, EXPOSURE_EV, "ev18"), win)
    l0, l1 = mean_luma(lo), mean_luma(hi)
    ev, im = 0.0, lo
    if l1 > l0 + 1:
        ev = max(EV_MIN, min(EV_MAX, (target - l0) * EXPOSURE_EV / (l1 - l0)))
        ev = max(EV_MIN, min(EV_MAX, ev + ev_delta))
        im = framed(develop(cr2, stock, ev, "evfit"), win)

    # If the develop clips where Canon's rendering did not, back off: a promo
    # cell with burnt ground is a defect even when the mean matches.
    for _ in range(3):
        if clipped(im) <= ref_clip + 1.0 or ev <= EV_MIN + 0.3:
            break
        ev -= 0.3
        im = framed(develop(cr2, stock, ev, "evclip"), win)

    print("  %-10s EV %.2f  luma %.0f (camera %.0f)  clip %.2f%% (camera %.2f%%)"
          % (frame, ev, mean_luma(im), target, clipped(im), ref_clip))
    return im.resize((CELL_W, round(im.height * CELL_W / im.width)), Image.LANCZOS)


def colour(im, name):
    """Numbers, so 'the colour looks right' is checkable."""
    hsv = im.convert("HSV")
    px = list(im.getdata())
    hpx = list(hsv.getdata())
    n = len(px)
    mr = sum(p[0] for p in px) / n
    mg = sum(p[1] for p in px) / n
    mb = sum(p[2] for p in px) / n
    sat = sum(p[1] for p in hpx) / n / 255 * 100
    lum = [0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2] for p in px]
    lum.sort()
    p95 = lum[int(n * 0.95)]
    clip = sum(1 for v in lum if v >= 250) / n * 100
    crush = sum(1 for v in lum if v <= 5) / n * 100
    print("  %-10s RGB %5.0f/%5.0f/%5.0f   sat %4.1f%%   p95 %5.1f   "
          "clip %4.2f%%  crush %5.2f%%" % (name, mr, mg, mb, sat, p95, clip, crush))


def main():
    card = sys.argv[1] if len(sys.argv) > 1 else "/Volumes/EOS_DIGITAL/DCIM"
    cells = []
    print("colour of each cell:")
    for frame, stock, label, ev_delta in STOCKS:
        im = cell(card, frame, stock, ev_delta)
        colour(im, frame)
        cells.append((im, label))

    cw = CELL_W
    ch = cells[0][0].height
    rows = (len(cells) + COLS - 1) // COLS
    W = COLS * cw + (COLS - 1) * GAP
    H = rows * (LABEL_H + ch) + (rows - 1) * GAP
    sheet = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(sheet)
    f = font(24)
    for i, (im, label) in enumerate(cells):
        x = (i % COLS) * (cw + GAP)
        y = (i // COLS) * (LABEL_H + ch + GAP)
        d.text((x + 6, y + 5), label, font=f, fill=INK)
        sheet.paste(im, (x, y + LABEL_H))
    out = os.path.join(OUT, "roll.jpg")
    sheet.save(out, quality=90, subsampling=0, optimize=True)
    print("roll.jpg  %dx%d  %.0f KB" % (sheet.width, sheet.height,
                                        os.path.getsize(out) / 1024))


if __name__ == "__main__":
    main()
