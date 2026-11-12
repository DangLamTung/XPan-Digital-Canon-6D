#!/usr/bin/env python3
"""Chart-first CIEDE2000 calibration of the 48-knot Cinestill 50D recipe.

Scoring ported from the branch's CALIBRATION_AND_SCORING_SPEC.md (its optimizer is hardwired
to macOS paths, so only the metric and the weights are reusable):

    domains   skin 3.5 | cyan/sea 3.0 | foliage 2.5 | highlights 2.5 | grays 2.0 | lips 1.5
    penalties gray balance (R-B)^2 on the ColorChecker gray row, highlight tint (B>R above Y>200)

Stage A corrects the colour chart alone -- the hue wheel -- with the curve frozen, because a
wrong wheel makes every later domain tweak meaningless. Stage B then nudges the domains with
the chart held as a hard term, and the curve only ever takes a black/white level offset.

    python tools/calib48.py --stage chart --iters 400
    python tools/calib48.py --stage domains --iters 300 --start chart_work/calib/chart.fs
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageFile

ImageFile.LOAD_TRUNCATED_IMAGES = True

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "algorithm"))
sys.path.insert(0, str(ROOT / "tools"))
from compare_export import srgb_to_lab  # noqa: E402

BIN = ROOT / "tools" / "build" / "grade48.exe"
SOURCE = Path(r"E:\fit_small")
DEH = Path(r"E:\dehancer_final\Cinestill-50D")
BASE = ROOT / "comparisons" / "xpan" / "stocks" / "Cinestill-50D"
CC = Path(r"E:\chart_cc\cc.jpg")
CC_REF = ROOT / "chart_cc_deh" / "Cinestill-50D" / "cc.jpg"
LIST = ROOT / "chart_work" / "eval102.txt"
WORK = ROOT / "chart_work" / "calib"
SECT = 48
KNOTS = [0, 32, 64, 96, 128, 160, 192, 224, 255]
SIZE = (320, 213)

WEIGHTS = {"skin": 3.5, "cyan": 3.0, "green": 2.5, "hi": 2.5, "gray": 2.0, "lips": 1.5}


def de00_vec(a, b):
    L1, a1, b1 = a[..., 0], a[..., 1], a[..., 2]
    L2, a2, b2 = b[..., 0], b[..., 1], b[..., 2]
    C1, C2 = np.hypot(a1, b1), np.hypot(a2, b2)
    Cb = 0.5 * (C1 + C2)
    G = 0.5 * (1 - np.sqrt(Cb ** 7 / (Cb ** 7 + 25.0 ** 7 + 1e-12)))
    a1p, a2p = a1 * (1 + G), a2 * (1 + G)
    C1p, C2p = np.hypot(a1p, b1), np.hypot(a2p, b2)
    h1p = np.degrees(np.arctan2(b1, a1p)) % 360
    h2p = np.degrees(np.arctan2(b2, a2p)) % 360
    dL, dC = L2 - L1, C2p - C1p
    dhp = h2p - h1p
    dhp = np.where(dhp > 180, dhp - 360, np.where(dhp < -180, dhp + 360, dhp))
    dhp = np.where((C1p * C2p) == 0, 0.0, dhp)
    dH = 2 * np.sqrt(C1p * C2p) * np.sin(np.radians(dhp) / 2)
    Lb, Cpb = 0.5 * (L1 + L2), 0.5 * (C1p + C2p)
    hb = np.where(np.abs(h1p - h2p) <= 180, 0.5 * (h1p + h2p),
                  np.where(h1p + h2p < 360, 0.5 * (h1p + h2p + 360), 0.5 * (h1p + h2p - 360)))
    T_ = (1 - 0.17 * np.cos(np.radians(hb - 30)) + 0.24 * np.cos(np.radians(2 * hb))
          + 0.32 * np.cos(np.radians(3 * hb + 6)) - 0.20 * np.cos(np.radians(4 * hb - 63)))
    SL = 1 + 0.015 * (Lb - 50) ** 2 / np.sqrt(20 + (Lb - 50) ** 2)
    SC, SH = 1 + 0.045 * Cpb, 1 + 0.015 * Cpb * T_
    RT = -2 * np.sqrt(Cpb ** 7 / (Cpb ** 7 + 25.0 ** 7 + 1e-12)) * np.sin(
        np.radians(60 * np.exp(-((hb - 275) / 25) ** 2)))
    return np.sqrt((dL / SL) ** 2 + (dC / SC) ** 2 + (dH / SH) ** 2 + RT * (dC / SC) * (dH / SH))


def read_num(path: Path, key: str) -> list[float]:
    for line in path.read_text(encoding="utf-8").splitlines():
        parts = line.split()
        if parts and parts[0].upper() == key:
            body = line[len(parts[0]):].strip()
            try:
                return [float(v) for v in body.replace(",", " ").split()]
            except ValueError:
                return []
    return []


def curve_lut(vals):
    return np.arange(256.0) if len(vals) < 4 else np.interp(np.arange(256), vals[0::2], vals[1::2])


def write_recipe(dst: Path, template: Path, curves, p):
    sat, rot, lum = p[0:SECT], p[SECT:2 * SECT], p[2 * SECT:3 * SECT]
    black, white = (p[3 * SECT], p[3 * SECT + 1]) if p.size >= 3 * SECT + 2 else (0.0, 0.0)
    out, seen = [], set()
    for line in template.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            out.append(line); continue
        k = line.split()[0].upper()
        seen.add(k)
        if k in curves:
            vals = []
            for x in KNOTS:
                y = curves[k][x] + (black if x == 0 else (white if x == 255 else 0.0))
                vals.append(f"{x},{int(round(np.clip(y, 0, 255)))}")
            out.append(f"{k:<9s} " + " ".join(vals))
        elif k == "HUE_SAT":
            out.append("HUE_SAT   " + ",".join(f"{v:.2f}" for v in sat))
        elif k == "HUE_ROT":
            out.append("HUE_ROT   " + ",".join(f"{v:.2f}" for v in rot))
        elif k == "HUE_LUM":
            out.append("HUE_LUM   " + ",".join(f"{v:.2f}" for v in lum))
        else:
            out.append(line)
    for k, v in (("HUE_SAT", sat), ("HUE_ROT", rot), ("HUE_LUM", lum)):
        if k not in seen:
            out.append(f"{k:<9s} " + ",".join(f"{x:.2f}" for x in v))
    dst.write_text("\n".join(out) + "\n", encoding="utf-8")


def smoothness(p):
    sat, rot, lum = p[0:SECT], p[SECT:2 * SECT], p[2 * SECT:3 * SECT]
    s = 0.0
    for arr, div in ((sat, 8.0), (lum, 6.0), (rot, 1.5)):
        d = np.diff(np.r_[arr, arr[0]]) / div
        s += float((d * d).sum())
    return s


class Calib:
    def __init__(self, names, curves, template, stage):
        self.names, self.curves, self.template, self.stage = names, curves, template, stage
        self.masks, self.refs, self.sizes = {}, {}, {}
        for n in names:
            sw, sh = Image.open(SOURCE / n).size
            sz = (SIZE[0], max(1, round(SIZE[0] * sh / sw)))
            self.sizes[n] = sz
            mm = np.asarray(Image.open(BASE / "masks" / (Path(n).stem + ".png"))
                            .convert("RGB").resize(sz, Image.NEAREST))
            self.masks[n] = {"skin": mm[..., 0] > 127, "green": mm[..., 1] > 127,
                             "blue": mm[..., 2] > 127}
            self.refs[n] = srgb_to_lab(np.asarray(Image.open(DEH / n).convert("RGB")
                                                  .resize(sz, Image.LANCZOS), dtype=np.float32))
        self.cc_ref = srgb_to_lab(np.asarray(Image.open(CC_REF).convert("RGB"), dtype=np.float32))
        self.cc_rgb_ref = np.asarray(Image.open(CC_REF).convert("RGB"), dtype=np.float32)

    def chart_term(self, p, tag):
        r = WORK / f"{tag}.fs"
        write_recipe(r, self.template, self.curves, p)
        j = WORK / f"{tag}_cc.jpg"
        subprocess.run([str(BIN), str(CC), str(r), str(j)], capture_output=True)
        if not j.exists():
            return 99.0, 99.0, 99.0
        lab = srgb_to_lab(np.asarray(Image.open(j).convert("RGB"), dtype=np.float32))
        d = de00_vec(lab, self.cc_ref)
        gray = np.array([18, 19, 20, 21, 22, 23])
        rgb = np.asarray(Image.open(j).convert("RGB"), dtype=np.float32)
        # gray balance on the six neutral patches, sampled on the chart grid
        P, GAP, COLS = 160, 12, 6
        gb = 0.0
        for gi, idx in enumerate(gray):
            row, col = divmod(int(idx), COLS)
            x = GAP + col * (P + GAP) + P // 2
            y = GAP + row * (P + GAP) + P // 2
            blk = rgb[y - 40:y + 40, x - 40:x + 40].reshape(-1, 3).mean(axis=0)
            gb += float((blk[0] - blk[2]) ** 2)
        gb /= len(gray)
        hi = 0.0
        for idx in (18, 19):
            row, col = divmod(int(idx), COLS)
            x = GAP + col * (P + GAP) + P // 2
            y = GAP + row * (P + GAP) + P // 2
            blk = rgb[y - 40:y + 40, x - 40:x + 40].reshape(-1, 3).mean(axis=0)
            hi += max(0.0, 0.0 - float(blk[2] - blk[0]))  # want B >= R in the highlights
        return float(d.mean()), gb, hi / 2.0

    def loss(self, p, batch, tag):
        chart, gb, hi_tint = self.chart_term(p, tag)
        smooth = smoothness(p)
        if self.stage == "chart":
            return chart + 0.02 * gb + 0.05 * hi_tint + 0.002 * smooth, {
                "chart": chart, "gray_bal": gb, "hi_tint": hi_tint, "total": chart}
        skin, cyan, green, hires, lips, gray = [], [], [], [], [], []
        r = WORK / f"{tag}.fs"
        write_recipe(r, self.template, self.curves, p)
        for n in batch:
            j = WORK / f"{tag}_{n}"
            subprocess.run([str(BIN), str(SOURCE / n), str(r), str(j)], capture_output=True)
            if not j.exists():
                continue
            lab = srgb_to_lab(np.asarray(Image.open(j).convert("RGB").resize(self.sizes[n], Image.LANCZOS),
                                         dtype=np.float32))
            ref = self.refs[n]
            d = de00_vec(lab, ref)
            m = self.masks[n]
            L = lab[..., 0]
            C = np.hypot(lab[..., 1], lab[..., 2])
            hue = np.degrees(np.arctan2(lab[..., 2], lab[..., 1]))
            sk = m["skin"]
            for acc, mask in ((skin, sk), (cyan, m["blue"] & (hue > -120) & (hue < -40)),
                              (green, m["green"]), (hires, L > 80),
                              (lips, sk & (C > 30) & (hue > -10) & (hue < 45)),
                              (gray, m["blue"] & (C < 6))):
                if mask.sum() >= 120:
                    acc.append(float(d[mask].mean()))
        mean = lambda v: float(np.mean(v)) if v else chart
        vals = {"chart": chart, "skin": mean(skin), "cyan": mean(cyan), "green": mean(green),
                "hi": mean(hires), "lips": mean(lips), "gray": mean(gray),
                "gray_bal": gb, "hi_tint": hi_tint}
        total = (WEIGHTS["skin"] * vals["skin"] + WEIGHTS["cyan"] * vals["cyan"]
                 + WEIGHTS["green"] * vals["green"] + WEIGHTS["hi"] * vals["hi"]
                 + WEIGHTS["gray"] * vals["gray"] + WEIGHTS["lips"] * vals["lips"]
                 + 2.0 * chart + 0.02 * gb + 0.05 * hi_tint + 0.002 * smooth)
        vals["total"] = float(total)
        return float(total), vals


def bounds():
    return (np.r_[np.full(SECT, 20.0), np.full(SECT, -35.0), np.full(SECT, 60.0), -8.0, -8.0],
            np.r_[np.full(SECT, 260.0), np.full(SECT, 35.0), np.full(SECT, 160.0), 8.0, 8.0])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stage", choices=("chart", "domains"), default="chart")
    ap.add_argument("--iters", type=int, default=300)
    ap.add_argument("--batch", type=int, default=12)
    ap.add_argument("--a", type=float, default=6.0)
    ap.add_argument("--c", type=float, default=1.2)
    ap.add_argument("--seed", type=int, default=11)
    ap.add_argument("--start", default="")
    ap.add_argument("--eval-every", type=int, default=20)
    args = ap.parse_args()

    WORK.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(args.seed)
    names = [l.strip() for l in LIST.read_text(encoding="utf-8-sig").splitlines() if l.strip()]
    start = Path(args.start) if args.start else Path(r"E:\xpan_wip\magiclantern\modules\film_sim\films\50D.fs")
    curves = {k: curve_lut(read_num(start, k)) for k in ("CURVE_R", "CURVE_G", "CURVE_B")}
    ev = Calib(names, curves, start, args.stage)
    p = np.r_[read_num(start, "HUE_SAT")[:SECT], read_num(start, "HUE_ROT")[:SECT],
              read_num(start, "HUE_LUM")[:SECT], 0.0, 0.0]
    lo, hi = bounds()

    base, vals = ev.loss(p, names[:args.batch], "baseline")
    print(f"stage {args.stage}  baseline {base:.2f}  " +
          "  ".join(f"{k} {v:.2f}" for k, v in vals.items()), flush=True)
    best, best_p = base, p.copy()
    for it in range(args.iters):
        batch = list(rng.choice(names, size=min(args.batch, len(names)), replace=False))
        c = args.c / (1 + it) ** 0.15
        delta = rng.choice([-1.0, 1.0], size=p.size)
        scale = np.r_[np.full(SECT, c), np.full(SECT, c * 0.15), np.full(SECT, c), c, c]
        lp, _ = ev.loss(np.clip(p + delta * scale, lo, hi), batch, f"p{it}")
        lm, _ = ev.loss(np.clip(p - delta * scale, lo, hi), batch, f"m{it}")
        g = (lp - lm) / (2 * c) * delta
        p = np.clip(p - (args.a / (1 + it * 0.04)) * g, lo, hi)
        if it % args.eval_every == args.eval_every - 1:
            loss, vals = ev.loss(p, names[:args.batch], f"i{it}")
            mark = ""
            if loss < best:
                best, best_p, mark = loss, p.copy(), " *"
                write_recipe(WORK / f"{args.stage}.fs", ev.template, ev.curves, p)
            print(f"iter {it + 1:4d} {loss:6.2f}{mark}  " +
                  "  ".join(f"{k} {v:.2f}" for k, v in vals.items()), flush=True)

    write_recipe(WORK / f"{args.stage}.fs", ev.template, ev.curves, best_p)
    loss, vals = ev.loss(best_p, names[:args.batch], "final")
    print(f"\nbest {loss:.2f} (from {base:.2f}) -> {WORK / f'{args.stage}.fs'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
