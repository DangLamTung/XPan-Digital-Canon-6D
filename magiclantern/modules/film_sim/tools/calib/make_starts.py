#!/usr/bin/env python3
"""Build candidate starting recipes for the 48-knot fit.

    neutral48.fs  every band unchanged (sat 100, rot 0, lum 100) -- no inherited grade
    cc48.fs       the ColorChecker-fitted tables from the 96-knot work, resampled to 48

A 96-value line is NOT resampled by the 48-knot engine (parse_hue_vec reads at most 48 values),
so the resample has to happen here or the table is silently truncated.
"""

import sys
from pathlib import Path

import numpy as np

ROOT = Path(r"C:\Users\Tung\Documents\Dehancer_Forensic_Evidence_2026-11-06")
SRC = Path(r"E:\xpan_wip\magiclantern\modules\film_sim\films\50D.fs")
CC96 = ROOT / "chart_work" / "cc" / "50D_cc.fs"
OUT = ROOT / "chart_work" / "gd48"
SECT = 48


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


def resample(vals: list[float], n: int) -> list[float]:
    if len(vals) == n:
        return vals
    v = np.array(vals, dtype=float)
    xp = np.r_[np.arange(len(v)) - len(v), np.arange(len(v)), np.arange(len(v)) + len(v)]
    fp = np.r_[v, v, v]
    return list(np.interp(np.arange(n) * len(v) / n, xp, fp))


def write(dst: Path, sat, rot, lum) -> None:
    out = []
    for line in SRC.read_text(encoding="utf-8").splitlines():
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


OUT.mkdir(parents=True, exist_ok=True)
write(OUT / "neutral48.fs", [100.0] * SECT, [0.0] * SECT, [100.0] * SECT)
print("neutral48.fs written")

if CC96.exists():
    sat = resample(read_num(CC96, "HUE_SAT"), SECT)
    rot = resample(read_num(CC96, "HUE_ROT"), SECT)
    lum = resample(read_num(CC96, "HUE_LUM"), SECT)
    write(OUT / "cc48.fs", sat, rot, lum)
    print(f"cc48.fs written  sat {min(sat):.0f}..{max(sat):.0f}  rot {min(rot):.1f}..{max(rot):.1f}")
else:
    print("no 96-knot CC recipe found, skipped cc48")
