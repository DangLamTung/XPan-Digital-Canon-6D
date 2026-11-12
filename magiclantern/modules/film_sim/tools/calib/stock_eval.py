#!/usr/bin/env python3
"""
Stock-agnostic CIEDE2000 corpus evaluation + ColorChecker QA.

For a .fs recipe and its Dehancer ground-truth corpus:
  * renders every frame at production resolution (6000x4000 -> 3000x2000, gscale=2)
  * pane-matches it against the Dehancer half of the comparison montage
  * reports whole-frame and segment errors, clipping, and a chart scorecard
  * writes metrics JSON, a comparison board and a chart board

Usage:
  stock_eval.py --stock "Portra 160" --fs films/P160.fs \
                --corpus ../xpan_comparisons/stocks/Portra-160 \
                --out ../renders/stocks/Portra-160
"""
import argparse, json, os, subprocess, sys, tempfile
import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = os.path.abspath(os.path.join(HERE, "../../../../.."))
sys.path.insert(0, HERE)
from tune_ciede2000 import srgb_to_lab, ciede2000

GRADE = os.path.join(BASE, "magiclantern/modules/film_sim/se/hosttest/grade_jpeg")
SRC = os.path.join(BASE, "batch110b")
CHART_SRC = os.path.join(BASE, "xpan_comparisons/charts/colorchecker_source.jpg")
EXCLUDE = {"087_IMG_4759.jpg"}          # night neon scene, not a daylight case
TRANS = {6: Image.Transpose.ROTATE_270, 8: Image.Transpose.ROTATE_90,
         3: Image.Transpose.ROTATE_180}

PATCH_NAMES = ["DarkSkin", "LightSkin", "BlueSky", "Foliage", "BlueFlower",
               "BluishGreen", "Orange", "PurplishBlue", "ModRed", "Purple",
               "YellowGreen", "OrangeYellow", "Blue", "Green", "Red", "Yellow",
               "Magenta", "Cyan", "White9.5", "Neutral8", "Neutral6.5",
               "Neutral5", "Neutral3.5", "Black2"]


def luma(x):
    return 0.299 * x[..., 0] + 0.587 * x[..., 1] + 0.114 * x[..., 2]


def render(path, fs, out):
    subprocess.run([GRADE, path, fs, out], capture_output=True, check=True)
    return Image.open(out).convert("RGB")


def load_source(fname):
    im = Image.open(os.path.join(SRC, fname))
    o = im.getexif().get(274, 1)
    if o in TRANS:
        im = im.transpose(TRANS[o])
    return im.convert("RGB")


def dehancer_pane(corpus, fname):
    im = Image.open(os.path.join(corpus, fname))
    w, _ = im.size
    return im.crop((w // 2, 0, w, 466)).convert("RGB")


def panes_for(corpus, fname, fs):
    """Render a frame at production resolution and pane-match it."""
    src = load_source(fname)
    tmp = tempfile.mktemp(suffix=".jpg")
    out = tempfile.mktemp(suffix=".jpg")
    src.save(tmp, quality=98)
    ml = render(tmp, fs, out)
    os.remove(tmp)
    os.remove(out)
    dh = dehancer_pane(corpus, fname)
    return ml.resize(dh.size, Image.Resampling.LANCZOS), dh


def score(ml, dh):
    a, b = np.array(ml, float), np.array(dh, float)
    de = ciede2000(srgb_to_lab(a), srgb_to_lab(b))
    out = {"mean": float(de.mean()), "median": float(np.median(de)),
           "p90": float(np.percentile(de, 90)), "max": float(de.max()),
           "blown": float((a >= 250).mean() * 100),
           "crushed": float((a <= 2).mean() * 100),
           "deh_blown": float((b >= 250).mean() * 100),
           "deh_crushed": float((b <= 2).mean() * 100)}
    return out, de


def segments(ml, dh, de):
    """Reuse the harness masks so every stock is measured the same way."""
    import eval_102_images as h
    global _LM
    try:
        _LM
    except NameError:
        _LM = h.init_landmarker()
    a, b = np.array(ml, float), np.array(dh, float)
    mask_skin, mask_lips, mask_hair = h.extract_face_segments(_LM, np.array(ml))
    m_sea, m_foliage, m_hi, m_shadow, m_mid = h.extract_general_segments(b)
    la, lb = srgb_to_lab(a), srgb_to_lab(b)
    res = {}
    for name, m in (("Skin", mask_skin), ("Lips", mask_lips), ("Hair", mask_hair),
                    ("Sea", m_sea), ("Foliage", m_foliage), ("Highlights", m_hi),
                    ("Midtones", m_mid), ("Shadows", m_shadow)):
        if np.count_nonzero(m) < 60:
            continue
        s = m > 0
        res[name] = {"de": float(de[s].mean()), "n": int(s.sum()),
                     "dL": float(la[..., 0][s].mean() - lb[..., 0][s].mean()),
                     "da": float(la[..., 1][s].mean() - lb[..., 1][s].mean()),
                     "db": float(la[..., 2][s].mean() - lb[..., 2][s].mean())}
    return res


def chart(fs, outdir, deh_chart=None):
    """Grade the ColorChecker: parity if a Dehancer chart exists, QA otherwise."""
    src = Image.open(CHART_SRC).convert("RGB")
    tmp = tempfile.mktemp(suffix=".jpg")
    out = tempfile.mktemp(suffix=".jpg")
    src.save(tmp, quality=98)
    ml = render(tmp, fs, out)
    os.remove(tmp)
    w, h = ml.size
    xs = np.linspace(w * 0.12, w * 0.88, 6)
    ys = np.linspace(h * 0.16, h * 0.84, 4)
    lab = srgb_to_lab(np.array(ml, float))
    rows = []
    for r in range(4):
        for c in range(6):
            x, y = int(xs[c]), int(ys[r])
            patch = lab[y - 10:y + 10, x - 10:x + 10].mean(axis=(0, 1))
            rows.append({"name": PATCH_NAMES[r * 6 + c], "L": float(patch[0]),
                         "a": float(patch[1]), "b": float(patch[2]),
                         "C": float(np.hypot(patch[1], patch[2]))})
    neutrals = rows[18:]
    result = {
        "mean_neutral_chroma": float(np.mean([p["C"] for p in neutrals])),
        "max_neutral_chroma": float(np.max([p["C"] for p in neutrals])),
        "ladder": [round(p["L"], 1) for p in neutrals],
        "ladder_monotonic": all(neutrals[i]["L"] > neutrals[i + 1]["L"]
                                for i in range(len(neutrals) - 1)),
        "patches": rows,
    }
    if deh_chart and os.path.exists(deh_chart):
        dh = Image.open(deh_chart).convert("RGB")
        if dh.size != ml.size:
            dh = dh.resize(ml.size, Image.Resampling.LANCZOS)
        b = srgb_to_lab(np.array(dh, float))
        des = []
        for r in range(4):
            for c in range(6):
                x, y = int(xs[c]), int(ys[r])
                p = b[y - 10:y + 10, x - 10:x + 10].mean(axis=(0, 1))
                m = lab[y - 10:y + 10, x - 10:x + 10].mean(axis=(0, 1))
                des.append(float(ciede2000(p.reshape(1, 1, 3), m.reshape(1, 1, 3))[0, 0]))
        result["parity_mean_de"] = float(np.mean(des))
        result["parity_max_de"] = float(np.max(des))
        result["parity_patches"] = {PATCH_NAMES[i]: round(d, 2) for i, d in enumerate(des)}

    # chart board: render beside source
    W = 720
    H = int(ml.size[1] * W / ml.size[0])
    board = Image.new("RGB", (W * 2 + 12, H + 46), (18, 19, 22))
    board.paste(src.resize((W, H), Image.Resampling.LANCZOS), (0, 46))
    board.paste(ml.resize((W, H), Image.Resampling.LANCZOS), (W + 12, 46))
    d = ImageDraw.Draw(board)
    try:
        f = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial Bold.ttf", 18)
    except OSError:
        f = ImageFont.load_default()
    d.text((8, 12), "SOURCE CHART", font=f, fill=(230, 230, 235))
    d.text((W + 20, 12), "STOCK RENDER", font=f, fill=(255, 208, 110))
    board.save(os.path.join(outdir, "chart.jpg"), quality=93)
    return result


def board(frames, outdir, stock):
    cols = [("001_IMG_6883.jpg", "portrait / skin"), ("015_IMG_8215.jpg", "skin + pavement"),
            ("008_IMG_7680.jpg", "foliage"), ("018_IMG_1553.jpg", "sea + sky")]
    tiles = []
    for f, note in cols:
        if f not in frames:
            continue
        ml, dh, _ = frames[f]
        tiles.append((ml, dh, f, note))
    if not tiles:
        return
    W, H = 640, 426
    sheet = Image.new("RGB", (W * 2 + 12, (H + 26) * len(tiles) + 48), (18, 19, 22))
    d = ImageDraw.Draw(sheet)
    try:
        fb = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial Bold.ttf", 22)
        fs = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", 15)
    except OSError:
        fb = fs = ImageFont.load_default()
    d.text((10, 12), f"{stock} — ML render (left) vs Dehancer (right)", font=fb, fill=(255, 208, 110))
    y = 48
    for ml, dh, f, note in tiles:
        sheet.paste(ml.resize((W, H), Image.Resampling.LANCZOS), (0, y + 26))
        sheet.paste(dh.resize((W, H), Image.Resampling.LANCZOS), (W + 12, y + 26))
        d.text((4, y + 4), f"{f} · {note}", font=fs, fill=(150, 155, 165))
        y += H + 26
    sheet.save(os.path.join(outdir, "board.jpg"), quality=92)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stock", required=True)
    ap.add_argument("--fs", required=True)
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--deh-chart", default=None)
    ap.add_argument("--limit", type=int, default=0)
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    files = [f for f in sorted(os.listdir(a.corpus)) if f not in EXCLUDE]
    if a.limit:
        files = files[:a.limit]

    frames, results, segacc = {}, [], {}
    for i, f in enumerate(files, 1):
        ml, dh = panes_for(a.corpus, f, a.fs)
        r, de = score(ml, dh)
        r["frame"] = f
        segs = segments(ml, dh, de)
        r["segs"] = {k: round(v["de"], 3) for k, v in segs.items()}
        for k, v in segs.items():
            segacc.setdefault(k, []).append(v)
        results.append(r)
        if f in ("001_IMG_6883.jpg", "015_IMG_8215.jpg", "008_IMG_7680.jpg", "018_IMG_1553.jpg"):
            frames[f] = (ml, dh, de)
        if i % 20 == 0 or i == len(files):
            print(f"  {a.stock}: {i}/{len(files)}  running mean {np.mean([x['mean'] for x in results]):.3f}", flush=True)

    means = [r["mean"] for r in results]
    summary = {
        "stock": a.stock, "fs": os.path.abspath(a.fs), "frames": len(results),
        "mean": float(np.mean(means)), "median": float(np.median(means)),
        "p90_frames": float(np.percentile(means, 90)),
        "min": float(np.min(means)), "max": float(np.max(means)),
        "blown": float(np.mean([r["blown"] for r in results])),
        "crushed": float(np.mean([r["crushed"] for r in results])),
        "deh_blown": float(np.mean([r["deh_blown"] for r in results])),
        "deh_crushed": float(np.mean([r["deh_crushed"] for r in results])),
        "under_4": int(sum(1 for m in means if m <= 4.0)),
        "above_6": int(sum(1 for m in means if m > 6.0)),
        "segments": {k: {"frames": len(v),
                         "de": round(float(np.mean([x["de"] for x in v])), 2),
                         "dL": round(float(np.mean([x["dL"] for x in v])), 1),
                         "da": round(float(np.mean([x["da"] for x in v])), 1),
                         "db": round(float(np.mean([x["db"] for x in v])), 1)}
                     for k, v in segacc.items()},
        "per_frame": results,
    }
    summary["chart"] = chart(a.fs, a.out, a.deh_chart)
    board(frames, a.out, a.stock)
    with open(os.path.join(a.out, "metrics.json"), "w") as fh:
        json.dump(summary, fh, indent=1)
    print(f"{a.stock}: mean {summary['mean']:.3f}  median {summary['median']:.3f}  "
          f"<=4 {summary['under_4']}/{summary['frames']}  chart neutral C* "
          f"{summary['chart']['mean_neutral_chroma']:.2f}")


if __name__ == "__main__":
    main()
