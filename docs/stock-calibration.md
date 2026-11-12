# Stock calibration — CineStill 50D and the rest of the catalogue

Every stock is measured with the same system the 50D audit used:

- **Corpus**: 101 of the 102 Dehancer comparison frames (night neon `087_IMG_4759.jpg` excluded
  as not a daylight case), graded at production resolution (6000×4000 → 3000×2000, `gscale=2`),
  pane-matched against the Dehancer 1.1.0 half of each montage.
- **Error**: whole-frame and per-segment CIEDE2000 (ISO/CIE 11664-6), plus highlight and shadow
  clipping counted against the reference.
- **ColorChecker**: 24-patch scorecard — parity where a Dehancer chart export exists (only
  CineStill 50D today), neutral-chroma and grayscale-ladder QA everywhere else.
- **Harness**: `magiclantern/modules/film_sim/tools/calib/stock_eval.py`; curve fit in the
  master-curve domain (see method below).

## Standings

| Stock | Baseline mean | Now | Median | ≤4.0 | Blown % (ours / Dehancer) |
|---|---|---|---|---|---|
| ColorPlus 200 | 13.69 | 3.94 | 4.04 | 49/101 | 0.14 / 2.03 |
| Portra 800 | 12.73 | 3.66 | 3.67 | 73/101 | 0.03 / 2.03 |
| Gold 200 | 12.50 | 3.96 | 3.91 | 58/101 | 0.00 / 0.03 |
| Pro 400H | 12.00 | 3.97 | 4.00 | 51/101 | 0.15 / 3.79 |
| Portra 400 | 11.97 | 4.07 | 4.08 | 44/101 | 0.06 / 0.01 |
| Kodachrome 64 | 11.76 | 6.26 | 6.30 | 1/101 | 3.57 / 3.87 |
| Ektar 100 | 10.95 | 5.42 | 5.21 | 15/101 | 0.16 / 2.81 |
| Provia 100F | 9.23 | 3.99 | 3.89 | 57/101 | 0.01 / 0.04 |
| Velvia 50 | 8.31 | 5.86 | 5.90 | 9/101 | 0.39 / 0.30 |
| HP5 Plus | 6.97 | 5.89 | 6.00 | 3/101 | 0.08 / 3.75 |
| Portra 160 | 6.94 | 4.97 | 4.87 | 22/101 | 8.26 / 6.84 |
| Acros 100 | 6.62 | 4.41 | 4.43 | 20/101 | 0.08 / 2.49 |
| Ektachrome E100 | 6.48 | 4.99 | 4.87 | 11/101 | 0.12 / 0.62 |
| XP2 Super | 4.38 | 1.78 | 1.66 | 100/101 | 0.02 / 0.73 |

Every stock with a Dehancer corpus improved. Largest moves: Portra 800 12.73 → 3.66,
ColorPlus 200 13.69 → 3.94, Gold 200 12.50 → 3.96, Pro 400H 12.00 → 3.97,
XP2 Super 4.38 → 1.78 (100/101 frames at ≤4.0).

## Method that worked

1. **Feed-forward tone fit, not residual.** The engine applies the master curve before the
   per-channel curves, so `curve_c` must be fitted on `master(source_c)`. A residual fit drifts
   and lifts blacks (Kodachrome 11.76 → 11.65 with neutral chroma rising to 14.9); the direct
   fit reached 6.26 in two iterations.
2. **Fit on panes, score on the production path.** Parameters fitted on 16 frames at pane
   geometry, then the full 101-frame corpus rendered at 3000×2000.
3. **Guard sparse bins** (<300 samples keep the previous value); without it, untouched
   high-end bins produced 60-level black lifts.

## Still open

- **Per-hue matrix pass** (48-sector `HUE_SAT`/`HUE_ROT`/`HUE_LUM`): untouched, and where the
  remaining error sits — Velvia 5.86, HP5 5.89, Ektar 5.42, Ektachrome 4.99, Portra 160 4.97.
- **Four corpora have no recipe**: CineStill-800T, Portra-160NC, Portra-160VC, Velvia-100.
- **ColorChecker parity only certified for 50D** — other stocks need a Dehancer chart export.
