#!/usr/bin/env python3
"""HSL tweak on top of the chart-corrected 48-knot recipe.

The chart is corrected first and then held: a move is rejected unless the domain score improves
AND the chart stays at or below its corrected value (+0.15 tolerance). Parameters are the eight
Lightroom band centres x (sat, rot, lum) = 24, applied over the 48 knots by interpolation, so
the wheel cannot tear into teeth.

Search is coordinate descent with acceptance (hill climbing): one parameter, two trial values,
keep only a strict improvement. It cannot diverge, which the SPSA stage did.

    python tools/tweak48.py --sweeps 4
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
from cc_compare import patches as cc_patches  # noqa: E402

BIN = ROOT / "tools" / "build" / "grade48.exe"
SOURCE = Path(r"E:\fit_small")
DEH = Path(r"E:\dehancer_final\Cinestill-50D")
BASE = ROOT / "comparisons" / "xpan" / "stocks" / "Cinestill-50D"
CC = Path(r"E:\chart_cc\cc.jpg")
CC_REF = ROOT / "chart_cc_deh" / "Cinestill-50D" / "cc.jpg"
LIST = ROOT / "chart_work" / "eval102.txt"
START = ROOT / "chart_work" / "fit48" / "best.fs"
WORK = ROOT / "chart_work" / "tweak"
SECT, BANDS = 48, 8
KNOTS = [0, 32, 64, 96, 128, 160, 192, 224, 255]
CENTRES = [0, 128, 256, 512, 768, 1024, 1152, 1280]
SIZE = (320, 213)
WEIGHTS = {"skin": 3.5, "cyan": 3.0, "green": 2.5, "hi": 2.5, "gray": 2.0, "lips": 1.5}
CHART_W = 2.0


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


def read_num(path: Path, key: str):
    for line in path.read_text(encoding="utf-8").splitlines():
        parts = line.split()
        if parts and parts[0].upper() == key:
            body = line[len(parts[0]):].strip()
            try:
                return [float(v) for v in body.replace(",", " ").split()]
            except ValueError:
                return []
    return []


def spread(band_vals):
    idx = [round(i * SECT / BANDS) for i in range(BANDS)]
    xp = np.r_[np.array(idx) - SECT, idx, np.array(idx) + SECT]
    fp = np.r_[band_vals, band_vals, band_vals]
    return np.interp(np.arange(SECT), xp, fp)


BASE_TBL = {k: np.array(read_num(START, k)) for k in ("HUE_SAT", "HUE_ROT", "HUE_LUM")}


def write_recipe(dst: Path, bands):
    sat = np.clip(BASE_TBL["HUE_SAT"] + spread(bands[0]), 0, 300)
    rot = np.clip(BASE_TBL["HUE_ROT"] + spread(bands[1]), -45, 45)
    lum = np.clip(BASE_TBL["HUE_LUM"] + spread(bands[2]), 0, 250)
    out = []
    for line in START.read_text(encoding="utf-8").splitlines():
        k = line.split()[0].upper() if line.split() else ""
        if k == "HUE_SAT":
            out.append("HUE_SAT   " + ",".join(f"{v:.2f}" for v in sat))
        elif k == "HUE_ROT":
            out.append("HUE_ROT   " + ",".join(f"{v:.2f}" for v in rot))
        elif k == "HUE_LUM":
            out.append("HUE_LUM   " + ",".join(f"{v:.2f}" for v in lum))
        else:
            out.append(line)
    dst.write_text("\n".join(out) + "\n", encoding="utf-8")


class Score:
    def __init__(self, names):
        self.names = names
        self.refs, self.masks, self.sizes = {}, {}, {}
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
        self.cc_ref = srgb_to_lab(cc_patches(CC_REF).astype(np.float32))

    def __call__(self, bands, tag):
        r = WORK / f"{tag}.fs"
        write_recipe(r, bands)
        cj = WORK / f"{tag}_cc.jpg"
        subprocess.run([str(BIN), str(CC), str(r), str(cj)], capture_output=True)
        chart = float(de00_vec(srgb_to_lab(cc_patches(cj).astype(np.float32)), self.cc_ref).mean())
        acc = {k: [] for k in ("skin", "cyan", "green", "hi", "lips", "gray")}
        for n in self.names:
            j = WORK / f"{tag}_{n}"
            subprocess.run([str(BIN), str(SOURCE / n), str(r), str(j)], capture_output=True)
            if not j.exists():
                continue
            lab = srgb_to_lab(np.asarray(Image.open(j).convert("RGB").resize(self.sizes[n], Image.LANCZOS),
                                         dtype=np.float32))
            d = de00_vec(lab, self.refs[n])
            m = self.masks[n]
            L = lab[..., 0]
            C = np.hypot(lab[..., 1], lab[..., 2])
            hue = np.degrees(np.arctan2(lab[..., 2], lab[..., 1]))
            sk = m["skin"]
            for acc_key, mask in (("skin", sk), ("cyan", m["blue"] & (hue > -120) & (hue < -40)),
                                  ("green", m["green"]), ("hi", L > 80),
                                  ("lips", sk & (C > 30) & (hue > -10) & (hue < 45)),
                                  ("gray", m["blue"] & (C < 6))):
                if mask.sum() >= 120:
                    acc[acc_key].append(float(d[mask].mean()))
        vals = {k: (float(np.mean(v)) if v else chart) for k, v in acc.items()}
        total = sum(WEIGHTS[k] * vals[k] for k in WEIGHTS) + CHART_W * chart
        vals.update(chart=chart, total=float(total))
        return float(total), vals


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sweeps", type=int, default=4)
    ap.add_argument("--images", type=int, default=24)
    ap.add_argument("--step", type=float, default=3.0)
    args = ap.parse_args()

    WORK.mkdir(parents=True, exist_ok=True)
    names = [l.strip() for l in LIST.read_text(encoding="utf-8-sig").splitlines() if l.strip()]
    rng = np.random.default_rng(3)
    subset = list(rng.choice(names, size=min(args.images, len(names)), replace=False))
    sc = Score(subset)

    bands = np.zeros((3, BANDS), dtype=float)   # deltas on top of the corrected table
    base, vals = sc(bands, "base")
    print(f"start {base:.2f}  chart {vals['chart']:.2f}  " +
          "  ".join(f"{k} {vals[k]:.2f}" for k in ("skin", "cyan", "green", "hi", "lips", "gray")), flush=True)
    chart_cap = vals["chart"] + 0.15
    best, best_bands = base, bands.copy()
    steps = {"sat": args.step, "rot": args.step * 0.5, "lum": args.step}

    for sw in range(args.sweeps):
        step = max(0.6, args.step * (0.6 ** sw))
        improved = 0
        for grp, key in ((0, "sat"), (1, "rot"), (2, "lum")):
            for b in range(BANDS):
                cur = bands[grp, b]
                for trial in (cur + step, cur - step):
                    trial = float(np.clip(trial, -10.0, 10.0))
                    cand = bands.copy()
                    cand[grp, b] = trial
                    tot, v = sc(cand, f"s{sw}{grp}{b}{int(trial)}")
                    if tot < best - 1e-3 and v["chart"] <= chart_cap:
                        best, best_bands = tot, cand.copy()
                        bands = cand
                        improved += 1
                        break
        print(f"sweep {sw + 1}  best {best:.2f}  step {step:.1f}  accepted {improved}", flush=True)
        if improved == 0 and sw >= 1:
            break

    write_recipe(WORK / "best.fs", best_bands)
    tot, v = sc(best_bands, "final")
    print(f"\nfinal {tot:.2f} (from {base:.2f})  chart {v['chart']:.2f}  " +
          "  ".join(f"{k} {v[k]:.2f}" for k in ("skin", "cyan", "green", "hi", "lips", "gray")))
    print(f"-> {WORK / 'best.fs'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
