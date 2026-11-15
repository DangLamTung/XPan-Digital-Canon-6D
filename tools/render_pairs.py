#!/usr/bin/env python3
"""Render every frame of a gt2048-style corpus as a side-by-side pair.

    ./tools/render_pairs.py --stock "Superia 200" --fs SUP200 \
        --gt gt2048/Fujicolor_Superia_200_+_3513/images --out ~/Desktop/superia200

Each file is `<frame>.jpg`: our render on the left, the Dehancer reference on the
right, labelled with the frame's median CIEDE2000. Contact sheets of six pairs are
written as `sheet-01.jpg`, `sheet-02.jpg`, ... for browsing.
"""
import argparse, os, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(BASE, "magiclantern/modules/film_sim/tools/calib"))
from tune_ciede2000 import srgb_to_lab, ciede2000

GRADE = os.path.join(BASE, "magiclantern/modules/film_sim/se/hosttest/grade_jpeg")
FILMS = os.path.join(BASE, "magiclantern/modules/film_sim/films")

FB = "/System/Library/Fonts/Supplemental/Arial Bold.ttf"
FR = "/System/Library/Fonts/Supplemental/Arial.ttf"


def orient_fix(im, tag):
    if tag == 8:
        return im.transpose(Image.ROTATE_90)
    if tag == 3:
        return im.transpose(Image.ROTATE_180)
    if tag == 6:
        return im.transpose(Image.ROTATE_270)
    return im


def pair(ours, ref, label, de, width):
    def fit(im):
        k = width / im.width
        return im.resize((width, max(1, int(im.height * k))), Image.LANCZOS)
    a, b = fit(ours), fit(ref)
    h = max(a.height, b.height)
    head = 26
    out = Image.new("RGB", (width * 2 + 4, h + head), (250, 249, 246))
    d = ImageDraw.Draw(out)
    f = ImageFont.truetype(FR, 15)
    out.paste(a, (0, head))
    out.paste(b, (width + 4, head))
    d.text((4, 5), "ours  |  " + label, font=f, fill=(28, 30, 34))
    d.text((width + 8, 5), "Dehancer  |  dE %.1f" % de, font=f, fill=(28, 30, 34))
    return out


def sheet(pairs, out_path, title, cols=2):
    """Fixed-height tiles: portrait and landscape pairs must line up in rows."""
    if not pairs:
        return
    tw, th = 900, 330
    ims = []
    for p, lab in pairs:
        k = min(tw / p.width, th / p.height)
        im = p.resize((max(1, int(p.width * k)), max(1, int(p.height * k))), Image.LANCZOS)
        tile = Image.new("RGB", (tw, th), (255, 255, 255))
        tile.paste(im, ((tw - im.width) // 2, (th - im.height) // 2))
        ims.append((tile, lab))
    rowh = th
    rows = (len(ims) + cols - 1) // cols
    head = 34
    W = cols * tw + (cols + 1) * 6
    H = head + rows * (rowh + 22)
    img = Image.new("RGB", (W, H), (250, 249, 246))
    d = ImageDraw.Draw(img)
    fb = ImageFont.truetype(FB, 20)
    f = ImageFont.truetype(FR, 14)
    d.text((8, 8), title, font=fb, fill=(28, 30, 34))
    for i, (im, lab) in enumerate(ims):
        cx = (i % cols) * (tw + 6) + 6
        cy = head + (i // cols) * (rowh + 22)
        img.paste(im, (cx, cy))
        d.text((cx, cy + im.height + 2), lab, font=f, fill=(110, 114, 122))
    img.save(out_path, quality=90)
    print("sheet:", os.path.basename(out_path), img.size)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stock", required=True)
    ap.add_argument("--fs", required=True)
    ap.add_argument("--gt", required=True)
    ap.add_argument("--src", default="batch110b")
    ap.add_argument("--out", required=True)
    ap.add_argument("--width", type=int, default=1000, help="width of each pane")
    ap.add_argument("--limit", type=int, default=0)
    a = ap.parse_args()

    fs = os.path.join(FILMS, a.fs if a.fs.endswith(".fs") else a.fs + ".fs")
    gt = os.path.join(BASE, a.gt) if not os.path.isabs(a.gt) else a.gt
    src = os.path.join(BASE, a.src) if not os.path.isabs(a.src) else a.src
    out = os.path.expanduser(a.out)
    if not os.path.isabs(out):
        out = os.path.join(BASE, out)
    os.makedirs(out, exist_ok=True)

    names = sorted(f for f in os.listdir(gt) if f.lower().endswith(".jpg"))
    if a.limit:
        names = names[:a.limit]

    scores, made = [], []
    for i, f in enumerate(names):
        s = os.path.join(src, f)
        if not os.path.exists(s):
            continue
        tmp = "/tmp/_pair_ml.jpg"
        r = subprocess.run([GRADE, s, fs, tmp], capture_output=True, text=True)
        if r.returncode != 0:
            print("  skip", f, r.stdout.strip()[:60])
            continue
        tag = Image.open(s).getexif().get(274, 1)
        ours = orient_fix(Image.open(tmp).convert("RGB"), tag)
        ref = Image.open(os.path.join(gt, f)).convert("RGB")
        aa = np.asarray(ours.resize(ref.size, Image.LANCZOS)).astype(np.float32) / 255.0
        bb = np.asarray(ref).astype(np.float32) / 255.0
        de = float(np.median(ciede2000(srgb_to_lab(aa).reshape(-1, 3),
                                       srgb_to_lab(bb).reshape(-1, 3))))
        scores.append(de)
        p = pair(ours, ref, a.stock, de, a.width)
        dst = os.path.join(out, f)
        p.save(dst, quality=90)
        made.append((p, "%s  dE %.1f" % (f, de)))
        if (i + 1) % 10 == 0:
            print("  %d/%d" % (i + 1, len(names)))

    if not scores:
        print("nothing rendered"); sys.exit(1)
    print("%s: %d pairs, median dE00 %.2f, p90 %.2f, max %.2f -> %s"
          % (a.stock, len(scores), float(np.median(scores)),
             float(np.percentile(scores, 90)), max(scores), out))

    made.sort(key=lambda t: -float(t[1].split("dE ")[1]))
    for k in range(0, len(made), 6):
        sheet(made[k:k + 6], os.path.join(out, "sheet-%02d.jpg" % (k // 6 + 1)),
              "%s — pairs, worst first (%d/%d)" % (a.stock, k // 6 + 1,
                                                   (len(made) + 5) // 6))


if __name__ == "__main__":
    main()
