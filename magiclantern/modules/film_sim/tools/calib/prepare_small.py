#!/usr/bin/env python3
"""Build upright, downscaled copies of the 102 evaluation images for fitting.

The originals are 6000x4000 with EXIF orientation 8 on the portrait frames. Resizing without
applying the orientation leaves every portrait frame landscape, and comparing that against
Dehancer's upright 4000x6000 render is what produced region errors of ~19 dE00.

Output carries no EXIF, so orientation is 1 and the aspect matches Dehancer's.
"""

import sys
from pathlib import Path

from PIL import Image, ImageOps

ROOT = Path(r"C:\Users\Tung\Documents\Dehancer_Forensic_Evidence_2026-11-06")
OUT = Path(r"E:\fit_small")
SOURCE = Path(r"E:\batch110b")
LONG = 700

names = [l.strip() for l in (ROOT / "chart_work" / "eval102.txt").read_text(encoding="utf-8-sig").splitlines() if l.strip()]
OUT.mkdir(parents=True, exist_ok=True)
done = 0
for n in names:
    src = SOURCE / n
    dst = OUT / n
    if dst.exists():
        done += 1
        continue
    im = ImageOps.exif_transpose(Image.open(src)).convert("RGB")
    w, h = im.size
    s = LONG / max(w, h)
    im.resize((max(1, int(w * s)), max(1, int(h * s))), Image.LANCZOS).save(dst, quality=92)
    done += 1
print(f"{done} upright copies in {OUT}")
