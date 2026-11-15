#!/usr/bin/env python3
"""Three-up comparison for one corpus frame: source, our recipe, Dehancer.

    ./tools/compare_one.py 001_IMG_6883.jpg Portra-400 portra

Sources are batch110b/, the Dehancer pane is the right half of the montage in
xpan_comparisons/stocks/<stock>/, and our pane is the module's own grade
(se/hosttest/grade_jpeg). It prints the frame's median CIEDE2000, the same
statistic docs/stock-calibration.md reports over the whole corpus.
"""
import os, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(BASE, "magiclantern/modules/film_sim/tools/calib"))
from tune_ciede2000 import srgb_to_lab, ciede2000

GRADE = os.path.join(BASE, "magiclantern/modules/film_sim/se/hosttest/grade_jpeg")


def main():
    if len(sys.argv) < 4:
        print(__doc__); sys.exit(1)
    frame, stock_dir, recipe = sys.argv[1], sys.argv[2], sys.argv[3]
    label = sys.argv[4] if len(sys.argv) > 4 else stock_dir.replace("-", " ")
    src = os.path.join(BASE, "batch110b", frame)
    mon = Image.open(os.path.join(BASE, "xpan_comparisons/stocks", stock_dir, frame)).convert("RGB")
    dh = mon.crop((mon.width // 2, 0, mon.width, mon.height))
    tmp = "/tmp/_cmp_ml.jpg"
    subprocess.run([GRADE, src, os.path.join(BASE, "magiclantern/modules/film_sim/films", recipe + ".fs"), tmp],
                   check=True, capture_output=True)
    ml = Image.open(tmp).convert("RGB")
    if ml.size != dh.size:
        ml = ml.resize(dh.size, Image.LANCZOS)
    a = np.asarray(ml).astype(np.float32) / 255.0
    b = np.asarray(dh).astype(np.float32) / 255.0
    de = ciede2000(srgb_to_lab(a).reshape(-1, 3), srgb_to_lab(b).reshape(-1, 3))
    print("%s  median dE00 %.2f  mean %.2f" % (frame, np.median(de), np.mean(de)))

    s = Image.open(src).convert("RGB").resize(dh.size, Image.LANCZOS)
    gap, head = 6, 40
    out = Image.new("RGB", (dh.width * 3 + gap * 2, dh.height + head), (250, 249, 246))
    d = ImageDraw.Draw(out)
    fb = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial Bold.ttf", 20)
    for i, (im, lab) in enumerate(zip((s, ml, dh),
                                     ("source (raw develop)", "ours — " + label,
                                      "Dehancer — " + label))):
        x = i * (dh.width + gap)
        out.paste(im, (x, head))
        d.text((x + 6, 10), lab, font=fb, fill=(28, 30, 34))
    name = os.path.join(BASE, "renders", "compare-%s-%s" % (stock_dir.lower(), frame))
    out.save(name, quality=93)
    print("wrote", name, out.size)


if __name__ == "__main__":
    main()
