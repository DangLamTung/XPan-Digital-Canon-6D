#!/usr/bin/env python3
"""Gradient descent on the 48-knot Cinestill 50D recipe, fitted to the 102-image evaluation set.

Evaluation set (the one defined earlier, unchanged):
    images   E:\\portra_final                       102 originals, 3000x2000
    masks    comparisons/xpan/stocks/Cinestill-50D/masks/<stem>.png    R=skin G=green B=blue
    refs     E:\\dehancer_final\\Cinestill-50D         Dehancer's render of each
    chart    chart_cc_deh/Cinestill-50D/cc.jpg        the 24-patch ColorChecker

    loss = 1.0*chart + 1.0*mean(skin_lo, skin_mid, skin_hi)
         + 0.6*mean(green, blue, red, cyan)
         + 0.005*smoothness(adjacent knots)          <- prevents the torn wheel

Gradient: SPSA -- two evaluations per iteration regardless of the 144 parameters. Each
iteration renders a random batch drawn from the 102, so the whole set drives the descent
without paying 102 renders per step. Final verification is over all 102.

    python tools/gd48.py --iters 80 --batch 12
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
SRC = Path(r"E:\xpan_wip\magiclantern\modules\film_sim\films\50D.fs")
SOURCE = Path(r"E:\fit_small")
DEH = Path(r"E:\dehancer_final\Cinestill-50D")
BASE = ROOT / "comparisons" / "xpan" / "stocks" / "Cinestill-50D"
CC = Path(r"E:\chart_cc\cc.jpg")
CC_REF = ROOT / "chart_cc_deh" / "Cinestill-50D" / "cc.jpg"
LIST = ROOT / "chart_work" / "eval102.txt"
WORK = ROOT / "chart_work" / "gd48"
SECT = 48
KNOTS = [0, 32, 64, 96, 128, 160, 192, 224, 255]
SIZE = (320, 213)
T = {6: Image.Transpose.ROTATE_270, 8: Image.Transpose.ROTATE_90, 3: Image.Transpose.ROTATE_180}


def de00_vec(a: np.ndarray, b: np.ndarray) -> np.ndarray:
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


def write_recipe(dst: Path, curves, p: np.ndarray) -> None:
    sat, rot, lum = p[0:SECT], p[SECT:2 * SECT], p[2 * SECT:3 * SECT]
    black, white = (p[3 * SECT], p[3 * SECT + 1]) if p.size >= 3 * SECT + 2 else (0.0, 0.0)
    out, seen = [], set()
    for line in SRC.read_text(encoding="utf-8").splitlines():
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


def smoothness(p: np.ndarray) -> float:
    sat, rot, lum = p[0:SECT], p[SECT:2 * SECT], p[2 * SECT:3 * SECT]
    s = 0.0
    for arr, div in ((sat, 8.0), (lum, 6.0), (rot, 1.5)):
        d = np.diff(np.r_[arr, arr[0]]) / div
        s += float((d * d).sum())
    return s


class Eval:
    def __init__(self, names: list[str], curves):
        self.names = names
        self.curves = curves
        self.masks, self.refs, self.sizes = {}, {}, {}
        for name in names:
            m = BASE / "masks" / (Path(name).stem + ".png")
            sw0, sh0 = Image.open(SOURCE / name).size
            sz0 = (SIZE[0], max(1, round(SIZE[0] * sh0 / sw0)))
            mm = np.asarray(Image.open(m).convert("RGB").resize(sz0, Image.NEAREST))
            self.masks[name] = {"skin": mm[..., 0] > 127, "green": mm[..., 1] > 127,
                                "blue": mm[..., 2] > 127}
            sw, sh = Image.open(SOURCE / name).size
            sz = (SIZE[0], max(1, round(SIZE[0] * sh / sw)))
            self.sizes[name] = sz
            self.refs[name] = srgb_to_lab(
                np.asarray(Image.open(DEH / name).convert("RGB").resize(sz, Image.LANCZOS),
                           dtype=np.float32))
        self.cc_ref = srgb_to_lab(np.asarray(Image.open(CC_REF).convert("RGB"), dtype=np.float32))

    def loss(self, p: np.ndarray, batch: list[str], tag: str) -> tuple[float, dict]:
        r = WORK / f"{tag}.fs"
        write_recipe(r, self.curves, p)
        cj = WORK / f"{tag}_cc.jpg"
        subprocess.run([str(BIN), str(CC), str(r), str(cj)], capture_output=True)
        if not cj.exists():
            return 1e6, {}
        chart = float(de00_vec(srgb_to_lab(np.asarray(Image.open(cj).convert("RGB"), dtype=np.float32)),
                               self.cc_ref).mean())
        acc = {k: [] for k in ("skin_lo", "skin_mid", "skin_hi", "green", "blue", "red", "cyan")}
        for name in batch:
            j = WORK / f"{tag}_{name}"
            subprocess.run([str(BIN), str(SOURCE / name), str(r), str(j)], capture_output=True)
            if not j.exists():
                continue
            im = Image.open(j).convert("RGB")
            lab = srgb_to_lab(np.asarray(im.resize(self.sizes[name], Image.LANCZOS), dtype=np.float32))
            d = de00_vec(lab, self.refs[name])
            m = self.masks[name]
            L, C = lab[..., 0], np.hypot(lab[..., 1], lab[..., 2])
            hue = np.degrees(np.arctan2(lab[..., 2], lab[..., 1]))
            for key, mask in (("skin_lo", m["skin"] & (L < 40)),
                              ("skin_mid", m["skin"] & (L >= 40) & (L < 62)),
                              ("skin_hi", m["skin"] & (L >= 62)),
                              ("green", m["green"]), ("blue", m["blue"]),
                              ("red", (C > 40) & (hue > -20) & (hue < 45)),
                              ("cyan", (C > 20) & (hue > -90) & (hue < -30))):
                if mask.sum() >= 150:
                    acc[key].append(float(d[mask].mean()))
        vals = {k: (float(np.mean(v)) if v else chart) for k, v in acc.items()}
        skin = np.mean([vals["skin_lo"], vals["skin_mid"], vals["skin_hi"]])
        land = np.mean([vals["green"], vals["blue"], vals["red"], vals["cyan"]])
        sm = smoothness(p)
        total = chart + skin + 0.6 * land + 0.005 * sm
        vals.update(chart=chart, skin=skin, land=land, smooth=sm, total=float(total))
        return float(total), vals


def bounds() -> tuple[np.ndarray, np.ndarray]:
    return (np.r_[np.full(SECT, 20.0), np.full(SECT, -35.0), np.full(SECT, 60.0), -10.0, -10.0],
            np.r_[np.full(SECT, 260.0), np.full(SECT, 35.0), np.full(SECT, 160.0), 10.0, 10.0])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--iters", type=int, default=80)
    ap.add_argument("--batch", type=int, default=12)
    ap.add_argument("--a", type=float, default=8.0)
    ap.add_argument("--c", type=float, default=1.5)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--eval-every", type=int, default=10)
    ap.add_argument("--start", default="", help="warm start from this recipe")
    args = ap.parse_args()

    WORK.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(args.seed)
    names = [l.strip() for l in LIST.read_text(encoding="utf-8-sig").splitlines() if l.strip()]
    curves = {k: curve_lut(read_num(SRC, k)) for k in ("CURVE_R", "CURVE_G", "CURVE_B")}
    ev = Eval(names, curves)
    global SRC
    if args.start:
        SRC = Path(args.start)
    p = np.r_[read_num(SRC, "HUE_SAT")[:SECT], read_num(SRC, "HUE_ROT")[:SECT],
              read_num(SRC, "HUE_LUM")[:SECT], 0.0, 0.0]
    if p.size < 3 * SECT:
        print(f"recipe holds {p.size} values, expected at least {3 * SECT}")
        return 2

    full = names[:]
    base_loss, base_vals = ev.loss(p, full, "baseline")
    print(f"baseline over {len(full)} images: total {base_loss:.2f}", flush=True)
    print("   " + "  ".join(f"{k} {v:.2f}" for k, v in base_vals.items()), flush=True)

    lo, hi = bounds()
    best = base_loss
    best_p = p.copy()
    m = np.zeros_like(p)
    v = np.zeros_like(p)
    b1, b2, eps = 0.9, 0.999, 1e-8
    lr = np.r_[np.full(SECT, 1.2), np.full(SECT, 0.15), np.full(SECT, 1.2)]
    for it in range(args.iters):
        batch = list(rng.choice(full, size=min(args.batch, len(full)), replace=False))
        c = args.c / (1.0 + it) ** 0.15
        delta = rng.choice([-1.0, 1.0], size=p.size)
        scale = np.r_[np.full(SECT, c), np.full(SECT, c * 0.15), np.full(SECT, c), c, c]
        lp, _ = ev.loss(np.clip(p + delta * scale, lo, hi), batch, f"p{it}")
        lm, _ = ev.loss(np.clip(p - delta * scale, lo, hi), batch, f"m{it}")
        # SPSA gradient: one number per iteration, spread over the perturbation direction
        g = (lp - lm) / (2.0 * c) * delta
        # scaled SPSA: the step follows the measured sensitivity, so a parameter with a
        # small gradient barely moves. Adam normalised everything to the same step and walked.
        lr_it = args.a / (1.0 + it * 0.05)
        p = np.clip(p - lr_it * g, lo, hi)
        if it % args.eval_every == args.eval_every - 1:
            loss, vals = ev.loss(p, full, f"i{it}")
            mark = ""
            if loss < best:
                best, best_p, mark = loss, p.copy(), " *"
            print(f"iter {it + 1:3d}  batch {lp:.2f}  full {loss:6.2f}{mark}  chart {vals['chart']:.2f}  "
                  f"skin {vals['skin']:.2f}  land {vals['land']:.2f}", flush=True)

    write_recipe(WORK / "best.fs", curves, best_p)
    loss, vals = ev.loss(best_p, full, "final")
    print(f"\nbaseline {base_loss:.2f} -> final {loss:.2f}")
    print("   " + "  ".join(f"{k} {v:.2f}" for k, v in vals.items()))
    print(f"   recipe: {WORK / 'best.fs'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
