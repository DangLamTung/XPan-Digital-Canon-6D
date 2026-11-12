#!/usr/bin/env python3
"""1:1 ColorChecker match by global coordinate descent with acceptance.

Per-patch bisection failed because patches share knots: both blues sit within one sector, so
each patch undid the other (dE00 8.25 -> 15.4). Here a candidate move is applied to one knot
and kept only if the mean dE00 over all 24 patches improves, so shared knots settle instead of
fighting.

Parameters: sat, rot, lum at each of the 24 patch hues (72), plus the master curve's knots for
the six greys, which the wheel cannot reach (the hue stage fades below chroma 20).

    python tools/chart_1to1.py --sweeps 6
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "algorithm"))
sys.path.insert(0, str(ROOT / "tools"))
from cc_compare import de00, patches  # noqa: E402
from compare_export import srgb_to_lab  # noqa: E402

BIN = ROOT / "tools" / "build" / "grade48.exe"
SRC = Path(r"E:\xpan_wip\magiclantern\modules\film_sim\films\50D.fs")
CC = Path(r"E:\chart_cc\cc.jpg")
CC_DEH = ROOT / "chart_cc_deh" / "Cinestill-50D" / "cc.jpg"
WORK = ROOT / "chart_work" / "one2one"
SECT = 48
KNOTS = [0, 32, 64, 96, 128, 160, 192, 224, 255]
DEG = 360.0 / SECT
NAMES = [l.split()[1] for l in Path(r"E:\chart_cc\cc.txt").read_text().splitlines()]


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


def write_recipe(dst, master, curves, sat, rot, lum):
    out = []
    for line in SRC.read_text(encoding="utf-8").splitlines():
        k = line.split()[0].upper() if line.split() else ""
        if k == "CURVE":
            out.append("CURVE     " + " ".join(f"{x},{int(round(master[x]))}" for x in KNOTS))
        elif k in curves:
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
    dst.write_text("\n".join(out) + "\n", encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sweeps", type=int, default=6)
    ap.add_argument("--step", type=float, default=4.0)
    ap.add_argument("--start", default=str(SRC))
    args = ap.parse_args()

    WORK.mkdir(parents=True, exist_ok=True)
    ref = srgb_to_lab(patches(CC_DEH).astype(np.float32))
    src_rgb = np.array([[int(v) for v in l.split()[-3:]]
                        for l in Path(r"E:\chart_cc\cc.txt").read_text().splitlines()])
    start = Path(args.start)
    curves = {k: curve_lut(read_num(start, k)) for k in ("CURVE_R", "CURVE_G", "CURVE_B")}
    master = curve_lut(read_num(start, "CURVE"))
    sat = np.array(read_num(start, "HUE_SAT"))[:SECT]
    rot = np.array(read_num(start, "HUE_ROT"))[:SECT]
    lum = np.array(read_num(start, "HUE_LUM"))[:SECT]

    counter = {"n": 0}

    def score(master, curves, sat, rot, lum):
        counter["n"] += 1
        r = WORK / "cur.fs"
        write_recipe(r, master, curves, sat, rot, lum)
        j = WORK / f"p{counter['n']:05d}.jpg"
        subprocess.run([str(BIN), str(CC), str(r), str(j)], capture_output=True)
        if not j.exists():
            raise SystemExit("render failed")
        lab = srgb_to_lab(patches(j).astype(np.float32))
        j.unlink(missing_ok=True)
        d = np.array([de00(lab[i], ref[i]) for i in range(24)])
        return float(d.mean()), lab, d

    best, lab, d = score(master, curves, sat, rot, lum)
    print(f"start  dE00 mean {best:5.2f} max {d.max():4.1f}", flush=True)

    # knots the patches actually occupy
    used = sorted({int(round((np.degrees(np.arctan2(lab[i, 2], lab[i, 1])) % 360.0) / DEG)) % SECT
                   for i in range(24) if np.hypot(lab[i, 1], lab[i, 2]) > 10})

    for sw in range(args.sweeps):
        step = max(0.5, args.step * (0.62 ** sw))
        improved = 0
        for k in used:
            for kind in ("sat", "rot", "lum"):
                for sgn in (1, -1):
                    if kind == "sat":
                        cand = sat.copy(); cand[k] = np.clip(cand[k] + sgn * step * 3, 5, 300)
                        v, l2, _ = score(master, curves, cand, rot, lum)
                    elif kind == "rot":
                        cand = rot.copy(); cand[k] = np.clip(cand[k] + sgn * step * 0.6, -45, 45)
                        v, l2, _ = score(master, curves, sat, cand, lum)
                    else:
                        cand = lum.copy(); cand[k] = np.clip(cand[k] + sgn * step * 3, 20, 300)
                        v, l2, _ = score(master, curves, sat, rot, cand)
                    if v < best - 1e-3:
                        best = v
                        if kind == "sat":
                            sat = cand
                        elif kind == "rot":
                            rot = cand
                        else:
                            lum = cand
                        lab = l2
                        improved += 1
                        break
        # greys on the curve, small steps
        for i in range(24):
            if np.hypot(ref[i, 1], ref[i, 2]) >= 10:
                continue
            x_in = float(np.mean(src_rgb[i]))
            pos = max(0, min(np.searchsorted(KNOTS, x_in) - 1, len(KNOTS) - 2))
            x0, x1 = KNOTS[pos], KNOTS[pos + 1]
            w0 = (x1 - x_in) / (x1 - x0) if x1 > x0 else 1.0
            # bisect the curve shift applied to both bracketing knots, on this grey's own
            # dE00 -- a +/-2 nudge was too small to move L* by 2..6 levels
            lo, hi = -24.0, 24.0
            base_patch = de00(lab[i], ref[i])
            for _ in range(10):
                mid = 0.5 * (lo + hi)
                nxt = master.copy()
                nxt[x0] = np.clip(nxt[x0] + mid * w0, 0, 255)
                nxt[x1] = np.clip(nxt[x1] + mid * (1 - w0), 0, 255)
                nxt = np.clip(np.maximum.accumulate(nxt), 0, 255)
                v, l2, _ = score(nxt, curves, sat, rot, lum)
                d_new = de00(l2[i], ref[i])
                if d_new < base_patch - 0.2:
                    master, lab, base_patch = nxt, l2, d_new
                    best = v
                    improved += 1
                    lo = mid if mid < 0 else lo
                    hi = mid if mid > 0 else hi
                else:
                    if mid < 0:
                        lo = mid
                    else:
                        hi = mid

        # per-channel neutralisation: a grey that is tinted cannot be fixed by the master
        # curve, which moves all three channels together
        for i in range(24):
            if np.hypot(ref[i, 1], ref[i, 2]) >= 10:
                continue
            x_in = float(np.mean(src_rgb[i]))
            pos = max(0, min(np.searchsorted(KNOTS, x_in) - 1, len(KNOTS) - 2))
            x0, x1 = KNOTS[pos], KNOTS[pos + 1]
            w0 = (x1 - x_in) / (x1 - x0) if x1 > x0 else 1.0
            for chan in ("CURVE_R", "CURVE_B"):
                lo, hi = -14.0, 14.0
                for _ in range(9):
                    mid = 0.5 * (lo + hi)
                    trial = {k: v.copy() for k, v in curves.items()}
                    bump = np.zeros(256)
                    bump[x0] = mid * w0
                    bump[x1] = mid * (1 - w0)
                    trial[chan] = np.clip(trial[chan] + bump, 0, 255)
                    v, l2, _ = score(master, trial, sat, rot, lum)
                    if v < best - 1e-3 or d_new < base_patch - 0.2:
                        curves, best, lab = trial, v, l2
                        improved += 1
                        if mid < 0:
                            lo = mid
                        else:
                            hi = mid
                    else:
                        if mid < 0:
                            lo = mid
                        else:
                            hi = mid

        # ---- joint tone move -------------------------------------------------
        # The greys are 2..5 L too bright and only the curve can reach them, but lowering
        # the curve darkens every colour too. So the two move together: curve down by d,
        # wheel luminance up by the same amount, then bisect d on the whole chart.
        lo, hi = -14.0, 14.0
        for _ in range(9):
            mid = 0.5 * (lo + hi)
            m2 = master.copy()
            for x in KNOTS:
                if 32 <= x <= 224:
                    m2[x] = np.clip(m2[x] - mid, 0, 255)
            m2 = np.clip(np.maximum.accumulate(m2), 0, 255)
            l2c = np.clip(lum + mid * 0.95, 20, 300)
            v, l3, _ = score(m2, curves, sat, rot, l2c)
            if v < best - 1e-3:
                master, lum, best, lab = m2, l2c, v, l3
                improved += 1
                if mid < 0:
                    lo = mid
                else:
                    hi = mid
            else:
                if mid < 0:
                    lo = mid
                else:
                    hi = mid
        d = np.array([de00(lab[i], ref[i]) for i in range(24)])
        print(f"sweep {sw + 1}  dE00 mean {best:5.2f} max {d.max():4.1f}  accepted {improved}", flush=True)
        if improved == 0 and sw >= 2:
            break

    write_recipe(WORK / "best.fs", master, curves, sat, rot, lum)
    best, lab, d = score(master, curves, sat, rot, lum)
    print(f"\nFINAL dE00 mean {d.mean():.2f} max {d.max():.1f}")
    for i, n in enumerate(NAMES):
        print(f"  {n:14s} dE00 {d[i]:4.2f}  dL {lab[i,0]-ref[i,0]:+5.1f}  "
              f"dC {np.hypot(lab[i,1],lab[i,2])-np.hypot(ref[i,1],ref[i,2]):+5.1f}")
    print(f"\n-> {WORK / 'best.fs'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
