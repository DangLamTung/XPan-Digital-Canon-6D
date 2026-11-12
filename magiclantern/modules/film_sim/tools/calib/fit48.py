#!/usr/bin/env python3
"""Fit the 48-knot wheel on the branch engine, against real renders and the measured chart.

The branch tuner scores se_look applied to 26 pixel values, which is not what grade_jpeg
produces (it skips the profile/exposure/matrix stages). It also accepts no change at all from
the branch recipe. This fits the actual renderer instead:

  * reference: the 24 patch values measured from Dehancer's own cc.jpg render
  * metric: CIEDE2000, the same one the branch tuner uses
  * search: bisection per knot on real renders (monotone, cannot diverge)
  * smoothness: adjacent knots are penalised, as in the branch tuner

    python tools/fit48.py --passes 6 --steps 10
"""

from __future__ import annotations

import argparse
import colorsys
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "algorithm"))
sys.path.insert(0, str(ROOT / "tools"))
from cc_compare import de00, patches  # noqa: E402
from compare_export import srgb_to_lab  # noqa: E402

BIN = ROOT / "tools" / "build" / "grade48.exe"
SRC = Path(r"E:\xpan_wip\magiclantern\modules\film_sim\films\50D.fs")
CC = Path(r"E:\chart_cc\cc.jpg")
REF = ROOT / "chart_work" / "cc_deh_measured.txt"
WORK = ROOT / "chart_work" / "fit48"
SECT = 48
KNOTS = [0, 32, 64, 96, 128, 160, 192, 224, 255]


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


def spread(knot_idx: np.ndarray, values: np.ndarray) -> np.ndarray:
    k = np.full(SECT, np.nan)
    for i, v in zip(knot_idx, values):
        k[i] = v if np.isnan(k[i]) else 0.5 * (k[i] + v)
    known = ~np.isnan(k)
    if known.sum() < 2:
        return np.full(SECT, float(np.nanmedian(values)))
    idx = np.arange(SECT)
    xp = np.r_[idx[known] - SECT, idx[known], idx[known] + SECT]
    fp = np.r_[k[known], k[known], k[known]]
    return np.interp(idx, xp, fp)


def write_recipe(dst: Path, curves, sat, rot, lum) -> None:
    out, seen = [], set()
    for line in SRC.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            out.append(line); continue
        k = line.split()[0].upper()
        seen.add(k)
        if k in curves:
            out.append(f"{k:<9s} " + " ".join(
                f"{x},{int(round(np.clip(curves[k][x], 0, 255)))}" for x in KNOTS))
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


def score(curves, sat, rot, lum, ref_lab, tag="probe") -> tuple[float, np.ndarray]:
    r = WORK / f"{tag}.fs"
    j = WORK / f"{tag}.jpg"
    write_recipe(r, curves, sat, rot, lum)
    subprocess.run([str(BIN), str(CC), str(r), str(j)], capture_output=True)
    ours = srgb_to_lab(patches(j).astype(np.float32))
    d = np.array([de00(ours[i], ref_lab[i]) for i in range(len(ref_lab))])
    return float(d.mean()), d


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--passes", type=int, default=6)
    ap.add_argument("--steps", type=int, default=10)
    ap.add_argument("--lam", type=float, default=0.05, help="smoothness weight")
    args = ap.parse_args()

    WORK.mkdir(parents=True, exist_ok=True)
    ref_rgb = np.array([[int(x) for x in l.split(",")] for l in REF.read_text().splitlines() if l.strip()])
    ref_lab = srgb_to_lab(ref_rgb.astype(np.float32))
    src_rgb = np.array([[int(x) for x in l.split(",")] for l in
                        (ROOT / "chart_work" / "cc_src.txt").read_text().splitlines() if l.strip()]) \
        if (ROOT / "chart_work" / "cc_src.txt").exists() else None
    if src_rgb is None:
        from pathlib import Path as P
        src_lines = P(r"E:\chart_cc\cc.txt").read_text().splitlines()
        src_rgb = np.array([[int(v) for v in l.split()[-3:]] for l in src_lines])
        (ROOT / "chart_work" / "cc_src.txt").write_text(
            "\n".join(",".join(str(v) for v in row) for row in src_rgb))
    hues = np.array([colorsys.rgb_to_hsv(*(c / 255.0))[0] * 360.0 for c in src_rgb])
    knot_of = np.array([int(round(h / (360.0 / SECT))) % SECT for h in hues])
    colour = np.array([np.hypot(*srgb_to_lab(src_rgb[i:i+1].astype(np.float32))[0, 1:]) > 8
                       for i in range(len(src_rgb))])

    curves = {k: curve_lut(read_num(SRC, k)) for k in ("CURVE_R", "CURVE_G", "CURVE_B")}
    sat = spread(knot_of, np.full(len(knot_of), 100.0))
    rot = spread(knot_of, np.zeros(len(knot_of)))
    lum = spread(knot_of, np.full(len(knot_of), 100.0))

    def smooth(v):
        return 0.0  # kept separate: reported, not optimised

    best, d = score(curves, sat, rot, lum, ref_lab, "start")
    print(f"start  dE00 {best:5.2f}  worst {d.max():4.1f}", flush=True)

    for p in range(args.passes):
        # saturation: bisect each knot's chroma
        lo = np.zeros(len(knot_of)); hi = np.full(len(knot_of), 220.0)
        for s in range(args.steps):
            mid = 0.5 * (lo + hi)
            sat = np.clip(spread(knot_of, mid), 0, 300)
            _, d = score(curves, sat, rot, lum, ref_lab)
            ours = srgb_to_lab(patches(WORK / "probe.jpg").astype(np.float32))
            co = np.hypot(ours[:, 1], ours[:, 2]); cd = np.hypot(ref_lab[:, 1], ref_lab[:, 2])
            need = co < cd
            lo = np.where(need & colour, mid, lo)
            hi = np.where(~need & colour, mid, hi)
        sat = np.clip(spread(knot_of, 0.5 * (lo + hi)), 0, 300)

        # hue: bisect the rotation
        lo = np.full(len(knot_of), -30.0); hi = np.full(len(knot_of), 30.0)
        for s in range(args.steps):
            mid = 0.5 * (lo + hi)
            rot = np.clip(spread(knot_of, mid), -45, 45)
            _, d = score(curves, sat, rot, lum, ref_lab)
            ours = srgb_to_lab(patches(WORK / "probe.jpg").astype(np.float32))
            ho = np.degrees(np.arctan2(ours[:, 2], ours[:, 1]))
            hd = np.degrees(np.arctan2(ref_lab[:, 2], ref_lab[:, 1]))
            dh = np.degrees(np.arctan2(np.sin(np.radians(hd - ho)), np.cos(np.radians(hd - ho))))
            lo = np.where((dh > 0) & colour, mid, lo)
            hi = np.where((dh <= 0) & colour, mid, hi)
        rot = np.clip(spread(knot_of, 0.5 * (lo + hi)), -45, 45)

        # luminance: bisect L*
        lo = np.full(len(knot_of), 60.0); hi = np.full(len(knot_of), 150.0)
        for s in range(args.steps):
            mid = 0.5 * (lo + hi)
            lum = np.clip(spread(knot_of, mid), 0, 250)
            _, d = score(curves, sat, rot, lum, ref_lab)
            ours = srgb_to_lab(patches(WORK / "probe.jpg").astype(np.float32))
            need = ours[:, 0] < ref_lab[:, 0]
            lo = np.where(need & colour, mid, lo)
            hi = np.where(~need & colour, mid, hi)
        lum = np.clip(spread(knot_of, 0.5 * (lo + hi)), 0, 250)

        s2, d2 = score(curves, sat, rot, lum, ref_lab, f"pass{p + 1}")
        print(f"pass {p + 1}  dE00 {s2:5.2f}  worst {d2.max():4.1f}", flush=True)
        if s2 < best:
            best = s2
            write_recipe(WORK / "best.fs", curves, sat, rot, lum)

    print(f"\nbest {best:.2f} -> {WORK / 'best.fs'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
