# Colour lattice and stock calibration

This document replaces the incorrect parts of `docs/film-stock-test-notes.md`.
Every number below is computed from `renders/stocks/*/metrics.json`
(the `per_frame` array), not copied from prose.

## 1. The lattice is 12 x 17 x 17

```c
/* magiclantern/modules/film_sim/se/engine/look.h */
#define SE_YUV3_NY 12      /* nodes along luma       */
#define SE_YUV3_NC 17      /* nodes along Cb, and Cr */

/* magiclantern/modules/film_sim/fs_core.h  (must match) */
#define FS_UV3_NY 12
#define FS_UV3_NC 17
```

So the table is **12 (Y) x 17 (Cb) x 17 (Cr)** = 3468 nodes, not "17x16x16".
A compile-time assert (`fs_lv3d_shape_check`) keeps the two definitions in step.

## 2. How the numbers are produced

* Rendered through the module's own engine, `se/hosttest/grade_jpeg` (the develop
  RGB path), at production resolution.
* Compared with the Dehancer reference of the same frame in CIEDE2000 over every
  pixel, taking the **median per frame**, then the median/mean across the corpus.
* Corpus: **101 frames** (`batch110b/` matched with the Dehancer corpus). The "102"
  in the old notes is wrong; only Superia 200 uses the `gt2048` corpus of 102.

Reproduce one stock:

```sh
./tools/eval_gt_stock.py --stock "Superia 200" --fs SUP200 \
    --gt gt2048/Fujicolor_Superia_200_+_3513/images --out renders/stocks/Superia-200
```

Each run writes `metrics.json` with a `per_frame` array: that is the source of the
table below.

## 3. How many stocks are calibrated: 20

Twenty distinct stocks have a scored `metrics.json` with at least 50 frames.
Several of them were tuned more than once (the files also hold older runs, and
`(cal v1)` style directories); the table keeps the **best** run per stock by
median.

| Stock | frames | mean dE00 | median dE00 | <=5.0 | <=6.0 |
|---|---|---|---|---|---|
| Xp2 Super | 101 | 1.12 | **0.99** | 100% | 100% |
| Acros 100 | 101 | 2.10 | **2.17** | 100% | 100% |
| Provia 100F | 101 | 2.65 | **2.57** | 100% | 100% |
| Gold 200 | 101 | 3.10 | **3.07** | 99% | 100% |
| Portra 160Vc | 101 | 3.23 | **3.10** | 95% | 99% |
| Hp5 Plus | 101 | 3.23 | **3.21** | 99% | 100% |
| Colorplus 200 | 101 | 3.20 | **3.21** | 99% | 100% |
| Portra 800 | 101 | 3.33 | **3.25** | 100% | 100% |
| Ektachrome E100 | 101 | 3.28 | **3.32** | 99% | 100% |
| Superia 200 | 102 | 3.49 | **3.33** | 96% | 99% |
| Portra 400 | 101 | 3.35 | **3.36** | 98% | 100% |
| Velvia 100 | 101 | 3.49 | **3.39** | 94% | 98% |
| Portra 160Nc | 101 | 3.61 | **3.56** | 92% | 99% |
| Cinestill 50D | 101 | 3.70 | **3.57** | 91% | 98% |
| Cinestill 800T | 101 | 3.76 | **3.74** | 98% | 100% |
| Ektar 100 | 101 | 3.88 | **3.76** | 85% | 96% |
| Pro 400H | 101 | 3.76 | **3.82** | 99% | 100% |
| Portra 160 | 101 | 3.96 | **3.90** | 86% | 97% |
| Velvia 50 | 101 | 4.16 | **3.99** | 75% | 92% |
| Kodachrome 64 | 101 | 4.72 | **4.80** | 65% | 96% |

Beyond these twenty, 26 more `.fs` recipes ship in
`magiclantern/modules/film_sim/films/` but have **no measured score yet** — they
are hand-written looks, including `classicneg`, which is why it is kept out of
calibrated comparisons.

## 4. Where the old notes are wrong

| The notes say | Reality | |
|---|---|---|
| the standard is `SE2_HUE` | no such symbol in any `.c/.h/.py/.fs` | wrong |
| lattice is `17x16x16` | `12x17x17` | wrong |
| corpus is `102` images | `101` (except Superia 200) | wrong |
| 50D median `4.07`, mean `4.51` | **3.57 / 3.70** | wrong |
| 50D `74.5%` at <=5.0 | **91.1%** | wrong |
| 50D `97.1%` at <=6.0 | **98.0%** | close |
| MediaPipe segmentation | `tools/calib/models/selfie_multiclass_256x256.*` and `import mediapipe` in `eval_102_images.py` | correct |
| per-zone dE00 (skin, lips, hair, sea, ...) | the script runs, but **writes no file** | unverifiable |

## 5. What would make the per-zone numbers trustworthy

`eval_102_images.py` prints its summary and discards it. Writing it to
`renders/stocks/<stock>/segments.json` next to `metrics.json` would make every
zone number (skin, lips, hair, foliage, sea, shadow, mid, highlight) a value that
can be checked instead of quoted.
