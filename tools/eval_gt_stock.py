#!/usr/bin/env python3
"""Score a recipe against a gt2048-style corpus (one reference image per frame).

The calibration harness (stock_eval.py) pane-matches Dehancer *montages*; some
stocks -- Superia among them -- only exist as `gt2048/<Stock>/images/*.jpg`
references. This does the same measurement for that layout:

    ./tools/eval_gt_stock.py --stock "Superia 200" --fs SUP200 \
        --gt gt2048/Fujicolor_Superia_200_+_3513/images --out renders/stocks/Superia-200

Renders every frame through the module's own develop colour path at production
resolution, compares it with the reference in CIEDE2000, and writes metrics.json
plus a board of the worst and best frames.
"""
import argparse, json, os, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageOps

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(BASE, "magiclantern/modules/film_sim/tools/calib"))
from tune_ciede2000 import srgb_to_lab, ciede2000

GRADE = os.path.join(BASE, "magiclantern/modules/film_sim/se/hosttest/grade_jpeg")
FILMS = os.path.join(BASE, "magiclantern/modules/film_sim/films")


def score(ours, ref):
    a = np.asarray(ours.resize(ref.size, Image.LANCZOS)).astype(np.float32) / 255.0
    b = np.asarray(ref).astype(np.float32) / 255.0
    d = ciede2000(srgb_to_lab(a).reshape(-1, 3), srgb_to_lab(b).reshape(-1, 3))
    return float(np.median(d)), float(np.mean(d))


def board(rows, out_path, title):
    if not rows:
        return
    cols = 4
    tw, th = 480, 320
    gap, head = 6, 44
    n = min(len(rows), 8)
    rows = rows[:n]
    W = cols * (tw * 2 + gap) + gap
    H = head + ((n + cols - 1) // cols) * (th + 26)
    img = Image.new("RGB", (W, H), (250, 249, 246))
    d = ImageDraw.Draw(img)
    fb = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial Bold.ttf", 21)
    fs = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", 14)
    d.text((gap, 12), title, font=fb, fill=(28, 30, 34))
    for i, (name, ours, ref, de) in enumerate(rows):
        cx = (i % cols) * (tw * 2 + gap) + gap
        cy = head + (i // cols) * (th + 26)
        for j, im in enumerate((ours, ref)):
            im = im.copy()
            im.thumbnail((tw, th), Image.LANCZOS)
            img.paste(im, (cx + j * (tw + 2), cy))
        d.text((cx, cy + th + 2), "%s  dE %.1f" % (name, de), font=fs, fill=(110, 114, 122))
    img.save(out_path, quality=92)
    print("board:", out_path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stock", required=True)
    ap.add_argument("--fs", required=True, help="recipe name in films/, with or without .fs")
    ap.add_argument("--gt", required=True, help="folder of reference images")
    ap.add_argument("--src", default="batch110b")
    ap.add_argument("--out", required=True)
    ap.add_argument("--limit", type=int, default=0)
    a = ap.parse_args()

    fs = a.fs if a.fs.endswith(".fs") else a.fs + ".fs"
    fs = os.path.join(FILMS, fs)
    gt = os.path.join(BASE, a.gt) if not os.path.isabs(a.gt) else a.gt
    src = os.path.join(BASE, a.src) if not os.path.isabs(a.src) else a.src
    out = os.path.join(BASE, a.out) if not os.path.isabs(a.out) else a.out
    os.makedirs(out, exist_ok=True)

    names = sorted(f for f in os.listdir(gt) if f.lower().endswith(".jpg"))
    if a.limit:
        names = names[:a.limit]
    per, keep = [], []
    for i, f in enumerate(names):
        s = os.path.join(src, f)
        if not os.path.exists(s):
            continue
        tmp = "/tmp/_gt_ml.jpg"
        r = subprocess.run([GRADE, s, fs, tmp], capture_output=True, text=True)
        if r.returncode != 0:
            print("  skip", f, r.stdout.strip()[:60])
            continue
        # The camera writes portrait frames as landscape pixels plus EXIF
        # orientation 8; the references are already upright. grade_jpeg ignores
        # EXIF, so without this every portrait frame is compared sideways and
        # scores 30 instead of 4.
        orient = Image.open(s).getexif().get(274, 1)
        ours = Image.open(tmp).convert("RGB")
        if orient == 8:
            ours = ours.transpose(Image.ROTATE_90)
        elif orient == 3:
            ours = ours.transpose(Image.ROTATE_180)
        elif orient == 6:
            ours = ours.transpose(Image.ROTATE_270)
        ref = Image.open(os.path.join(gt, f)).convert("RGB")
        med, mean = score(ours, ref)
        per.append({"frame": f, "median": med, "mean": mean})
        keep.append((f, ours, ref, med))
        if (i + 1) % 10 == 0:
            print("  %d/%d" % (i + 1, len(names)))

    if not per:
        print("nothing scored"); sys.exit(1)
    med = float(np.median([p["median"] for p in per]))
    mean = float(np.mean([p["median"] for p in per]))
    p90 = float(np.percentile([p["median"] for p in per], 90))
    res = {"stock": a.stock, "fs": os.path.basename(fs), "frames": len(per),
           "median": med, "mean_of_medians": mean, "p90": p90,
           "min": min(p["median"] for p in per), "max": max(p["median"] for p in per),
           "per_frame": per}
    json.dump(res, open(os.path.join(out, "metrics.json"), "w"), indent=1)
    print("%s: %d frames, median dE00 %.2f (p90 %.2f, max %.2f)"
          % (a.stock, len(per), med, p90, res["max"]))

    keep.sort(key=lambda t: -t[3])
    board(keep[:8], os.path.join(out, "board-worst.jpg"),
          "%s — worst frames (dE00 vs Dehancer)" % a.stock)
    keep.sort(key=lambda t: t[3])
    board(keep[:8], os.path.join(out, "board-best.jpg"),
          "%s — best frames (dE00 vs Dehancer)" % a.stock)


if __name__ == "__main__":
    main()
