#!/usr/bin/env python3
"""Score a recipe on the wip-experiment engine: photo dE00 plus the region split.

Masks come from the existing per-image masks (R=skin, G=green, B=blue), so the same pixels are
compared for both renders.

    python tools/eval_branch.py --recipe E:/xpan_wip/.../50D.fs --tag branch50d
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
from score_stocks import patch_delta  # noqa: E402
from stocks import STOCKS  # noqa: E402

BIN = ROOT / "tools" / "build" / "wip_grade.exe"
SOURCE = Path(r"E:\batch110b")
DEH = Path(r"E:\dehancer_final")
WORK = ROOT / "chart_work" / "branch_eval"
T = {6: Image.Transpose.ROTATE_270, 8: Image.Transpose.ROTATE_90, 3: Image.Transpose.ROTATE_180}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--stock", default="Cinestill 50D")
    ap.add_argument("--recipe", required=True)
    ap.add_argument("--tag", default="branch")
    ap.add_argument("--bin", default=str(BIN))
    args = ap.parse_args()

    meta = STOCKS[args.stock]
    base = ROOT / "comparisons" / "xpan" / "stocks" / args.stock.replace(" ", "-")
    masks = base / "masks"
    WORK.mkdir(parents=True, exist_ok=True)
    names = sorted(p.name for p in base.joinpath("renders").glob("*.jpg"))
    recipe = Path(args.recipe)
    bin_path = Path(args.bin)

    rows = []
    for name in names:
        out = WORK / f"{args.tag}_{name}"
        subprocess.run([str(bin_path), str(SOURCE / name), str(recipe), str(out)], capture_output=True)
        if not out.exists():
            continue
        o = Image.open(SOURCE / name).getexif().get(274, 1)
        im = Image.open(out).convert("RGB")
        if o in T:
            im = im.transpose(T[o])
        size = (420, 280)
        a = np.asarray(im.resize(size, Image.LANCZOS), dtype=np.float32)
        b = np.asarray(Image.open(DEH / meta["dehancer"] / name).convert("RGB").resize(size, Image.LANCZOS),
                       dtype=np.float32)
        la, lb = srgb_to_lab(a), srgb_to_lab(b)
        de = float(np.array(patch_delta(la, lb, grid=12)).mean())

        m = Image.open(masks / (Path(name).stem + ".png")).convert("RGB").resize(size, Image.NEAREST)
        m = np.asarray(m)
        skin_all = m[..., 0] > 127
        green = m[..., 1] > 127
        blue = m[..., 2] > 127
        L = la[..., 0]
        skin_mid = skin_all & (L > 35) & (L < 70)
        C = np.hypot(la[..., 1], la[..., 2])
        hue = np.degrees(np.arctan2(la[..., 2], la[..., 1]))
        red = (C > 40) & (hue > -20) & (hue < 45) & (L > 20) & (L < 75)
        cyan = (C > 20) & (hue > -90) & (hue < -30) & (L > 30)

        row = {"image": name, "dE00": de}
        for key, mask in (("skin_mid", skin_mid), ("green", green), ("blue", blue),
                          ("red", red), ("cyan", cyan)):
            if mask.sum() < 150:
                continue
            d = (la - lb)[mask]
            row[f"{key}_dL"] = float(d[:, 0].mean())
            row[f"{key}_dC"] = float((np.hypot(la[..., 1], la[..., 2]) - np.hypot(lb[..., 1], lb[..., 2]))[mask].mean())
            row[f"{key}_dhue"] = float((np.degrees(np.arctan2(la[..., 2], la[..., 1]))
                                        - np.degrees(np.arctan2(lb[..., 2], lb[..., 1])))[mask].mean())
        rows.append(row)

    print(f"{args.tag}: {len(rows)} images")
    print(f"  photo dE00 mean {np.mean([r['dE00'] for r in rows]):.2f}  "
          f"median {np.median([r['dE00'] for r in rows]):.2f}")
    for key in ("skin_mid", "green", "blue", "red", "cyan"):
        vals = [r for r in rows if f"{key}_dL" in r]
        if not vals:
            continue
        print(f"  {key:9s} n={len(vals):3d}  dL {np.median([v[f'{key}_dL'] for v in vals]):+5.1f}  "
              f"dC {np.median([v[f'{key}_dC'] for v in vals]):+5.1f}  "
              f"dhue {np.median([v[f'{key}_dhue'] for v in vals]):+5.1f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
