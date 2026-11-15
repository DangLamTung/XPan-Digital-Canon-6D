#!/usr/bin/env python3
"""Apply the calibrated film stocks to the Da Lat frames and build a sheet.

The stocks are the ones docs/stock-calibration.md lists as verified, rendered
through the module's own develop colour path (se/hosttest/grade_jpeg), which is
the same code the camera runs -- not a Python approximation.

    ./tools/dalat_calib_sheet.py [--out renders/dalat-calib]

Writes one folder per stock plus sheet-<n>.jpg contact sheets.
"""
import argparse, os, subprocess, sys
from PIL import Image, ImageDraw, ImageFont

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GRADE = os.path.join(BASE, "magiclantern/modules/film_sim/se/hosttest/grade_jpeg")
FILMS = os.path.join(BASE, "magiclantern/modules/film_sim/films")
SRC = os.path.join(BASE, "samples/dalat")

# recipe -> label. Ordered by the verified CIEDE2000 median, best first.
STOCKS = [
    ("XP2",           "XP2 Super"),
    ("acros",         "Acros 100"),
    ("provia",        "Provia 100F"),
    ("HP5",           "HP5 Plus"),
    ("CP200",         "ColorPlus 200"),
    ("gold",          "Gold 200"),
    ("EK100",         "Ektar 100"),
    ("P160",          "Portra 160"),
    ("portra",        "Portra 400"),
    ("velvia",        "Velvia 50"),
    ("400H",          "Pro 400H"),
]

# Recipes with no verified score in docs/stock-calibration.md. They are shown only
# with --include-unverified, because the sheet otherwise claims to be a comparison
# of calibrated stocks and Classic Neg has never been measured against a reference:
# there is no Dehancer corpus for it in xpan_comparisons/stocks or gt2048.
UNVERIFIED = [
    ("classicneg",    "Classic Neg (uncalibrated)"),
]
TILE_W = 380
LABEL_H = 22
PAD = 8


def photos():
    out = []
    for fn in sorted(os.listdir(SRC)):
        if fn.lower().endswith(".jpg") and fn[0].isdigit() and "look-sheet" not in fn:
            out.append(fn)
    return out


def mean_rgb(path):
    from PIL import ImageStat
    return sum(ImageStat.Stat(Image.open(path).convert("RGB")).mean) / 3.0


def recipe_with_exposure(stock, ev, tmpdir):
    src = os.path.join(FILMS, stock + ".fs")
    txt = open(src, encoding="utf-8", errors="ignore").read()
    out = os.path.join(tmpdir, "%s_%+.1f.fs" % (stock, ev))
    with open(out, "w", encoding="utf-8") as f:
        f.write(txt)
        if ev:
            f.write("\nEXPOSURE  %.2f\n" % ev)
    return out


def match_exposure(stock, probe_photo, target, tmpdir):
    """Pick the exposure that lands the graded mean on the source's mean.

    The stocks are raw-domain looks; the samples here are already developed
    JPEGs, so without this every stock comes out about a stop dark. Matching the
    mean makes the sheet a colour comparison instead of a brightness one.
    """
    best, best_err = 0.0, 1e9
    for ev in (0.0, 0.4, 0.8, 1.2, 1.6):
        fs = recipe_with_exposure(stock, ev, tmpdir)
        out = os.path.join(tmpdir, "probe.jpg")
        r = subprocess.run([GRADE, probe_photo, fs, out], capture_output=True)
        if r.returncode != 0:
            continue
        err = abs(mean_rgb(out) - target)
        if err < best_err:
            best, best_err = ev, err
    return best


def render(photo, stock, dst, ev=0.0, tmpdir=None):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    fs = (recipe_with_exposure(stock, ev, tmpdir) if ev else
          os.path.join(FILMS, stock + ".fs"))
    if not os.path.exists(fs):
        return False
    r = subprocess.run([GRADE, os.path.join(SRC, photo), fs, dst],
                       capture_output=True, text=True)
    return r.returncode == 0 and os.path.exists(dst)


def sheet(rows, out_path, title):
    if not rows:
        return
    ncol = len(rows[0][1]) + 1
    nrow = len(rows)
    colw = [TILE_W] + [TILE_W] * (ncol - 1)
    row_h = int(TILE_W / 2.7) + LABEL_H + PAD
    header = 46
    W = sum(colw) + PAD * (ncol + 1)
    H = header + nrow * row_h + PAD
    img = Image.new("RGB", (W, H), (250, 249, 246))
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", 17)
    fb = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial Bold.ttf", 22)
    d.text((PAD, 14), title, font=fb, fill=(28, 30, 34))
    x = PAD
    for j in range(ncol):
        label = "no look" if j == 0 else rows[0][2][j - 1]
        d.text((x + 4, header - 6), label, font=f, fill=(70, 74, 82))
        x += colw[j] + PAD
    y = header + 18
    for name, tiles, _labels in rows:
        x = PAD
        for j, path in enumerate([name] + tiles):
            src = path if j else os.path.join(SRC, name)
            try:
                im = Image.open(src).convert("RGB")
            except Exception:
                x += colw[j] + PAD
                continue
            th = int(TILE_W / 2.7)
            im = im.resize((TILE_W, int(TILE_W * im.height / im.width)), Image.LANCZOS)
            if im.height > th:
                top = (im.height - th) // 2
                im = im.crop((0, top, TILE_W, top + th))
            img.paste(im, (x, y))
            d.rectangle([x, y, x + TILE_W - 1, y + im.height - 1], outline=(210, 210, 210))
            d.text((x + 4, y + im.height + 3), name.replace(".jpg", ""), font=f,
                   fill=(110, 114, 122))
            x += colw[j] + PAD
        y += row_h
    img.save(out_path, quality=92)
    print("wrote", out_path, img.size)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="renders/dalat-calib")
    ap.add_argument("--stocks", default="", help="comma list of recipe names")
    ap.add_argument("--include-unverified", action="store_true",
                    help="also render stocks with no measured score (Classic Neg)")
    a = ap.parse_args()
    out = os.path.join(BASE, a.out) if not os.path.isabs(a.out) else a.out
    os.makedirs(out, exist_ok=True)

    stocks = list(STOCKS)
    if a.include_unverified:
        stocks += UNVERIFIED
    if a.stocks:
        want = [s.strip() for s in a.stocks.split(",") if s.strip()]
        stocks = [(s, s) for s in want]

    ph = photos()
    print("%d photos x %d stocks" % (len(ph), len(stocks)))
    rendered = {s: [] for s, _ in stocks}
    import tempfile
    tmpdir = tempfile.mkdtemp(prefix="dalatcal_")
    probe = os.path.join(SRC, ph[0])
    target = mean_rgb(probe)
    print("brightness target (mean) %.1f from %s" % (target, ph[0]))
    ev_of = {}
    for s, label in stocks:
        ev_of[s] = match_exposure(s, probe, target, tmpdir)
        print("  %-12s exposure %+.1f" % (s, ev_of[s]))
    ok = 0
    for s, _label in stocks:
        for p in ph:
            dst = os.path.join(out, s, p)
            if render(p, s, dst, ev_of[s], tmpdir):
                ok += 1
            else:
                print("  failed:", s, p)
    print("rendered %d/%d" % (ok, len(ph) * len(stocks)))

    # split into sheets of 5 stocks so each stays readable
    labels = [l for _, l in stocks]
    step = 5
    n = 0
    for start in range(0, len(stocks), step):
        chunk = stocks[start:start + step]
        rows = []
        for p in ph:
            tiles = [os.path.join(out, s, p) for s, _ in chunk]
            rows.append((p, tiles, [l for _, l in chunk]))
        n += 1
        sheet(rows, os.path.join(out, "sheet-%d.jpg" % n),
              "Đà Lạt, no look then the calibrated stocks (%d/%d)" % (n, (len(stocks) + step - 1) // step))


if __name__ == "__main__":
    main()
