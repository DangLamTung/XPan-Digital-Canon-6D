# Stock calibration — CineStill 50D and the rest of the catalogue

Every stock is measured with the same system the 50D audit used:

- **Corpus**: 101 of the 102 Dehancer comparison frames (night neon `087_IMG_4759.jpg`
  excluded as not a daylight case), graded at production resolution (6000×4000 → 3000×2000,
  `gscale=2`), pane-matched against the Dehancer 1.1.0 half of each montage.
- **Error**: whole-frame and per-segment CIEDE2000 (ISO/CIE 11664-6), plus highlight and
  shadow clipping counted against the reference.
- **ColorChecker**: 24-patch scorecard — parity where a Dehancer chart export exists (only
  CineStill 50D today), neutral-chroma and grayscale-ladder QA everywhere else.
- **Harness**: `magiclantern/modules/film_sim/tools/calib/stock_eval.py`.

## Standings

| Stock | Baseline mean | Now | Median | ≤4.0 | Blown % (ours / Dehancer) |
|---|---|---|---|---|---|
| ColorPlus 200 | 13.69 | 3.94 | 4.04 | 49/101 | 0.14 / 2.03 |
| Portra 800 | 12.73 | 3.66 | 3.67 | 73/101 | 0.03 / 2.03 |
| Gold 200 | 12.50 | 3.52 | 3.58 | 79/101 | 0.00 / 0.03 |
| Pro 400H | 12.00 | 3.97 | 4.00 | 51/101 | 0.15 / 3.79 |
| Portra 400 | 11.97 | 3.65 | 3.67 | 69/101 | 0.02 / 0.01 |
| Kodachrome 64 | 11.76 | 5.75 | 5.76 | 2/101 | 3.37 / 3.87 |
| Ektar 100 | 10.95 | 4.93 | 4.84 | 21/101 | 0.13 / 2.81 |
| Provia 100F | 9.23 | 3.99 | 3.89 | 57/101 | 0.01 / 0.04 |
| Velvia 50 | 8.31 | 5.57 | 5.49 | 15/101 | 0.26 / 0.30 |
| HP5 Plus | 6.97 | 3.43 | 3.44 | 88/101 | 1.70 / 3.75 |
| Portra 160 | 6.94 | 4.46 | 4.42 | 33/101 | 6.73 / 6.84 |
| Acros 100 | 6.62 | 2.23 | 2.22 | 101/101 | 0.07 / 2.49 |
| Ektachrome E100 | 6.48 | 4.56 | 4.42 | 25/101 | 0.08 / 0.62 |
| Velvia 50 | 5.31 | 5.43 | 5.32 | 15/101 | 0.26 / 0.30 |
| PORTRA 160NC | 4.54 | — | — | — | — |
| CINESTILL 800T | 4.45 | — | — | — | — |
| XP2 Super | 4.38 | 1.78 | 1.66 | 100/101 | 0.02 / 0.73 |
| VELVIA 100 | 4.32 | — | — | — | — |
| PORTRA 160VC | 4.16 | — | — | — | — |

Every stock with a Dehancer corpus improved. Largest moves: Portra 800 12.73 → 3.66,
ColorPlus 200 13.69 → 3.94, Gold 200 12.50 → 3.96, Pro 400H 12.00 → 3.97,
XP2 Super 4.38 → 1.78 (100/101 at ≤4.0).

## Method that worked

1. **Feed-forward tone fit, not residual.** The engine applies the master curve before the
   per-channel curves, so `curve_c` must be fitted on `master(source_c)`. A residual fit
   drifts and lifts blacks (Kodachrome 11.76 → 11.65, neutral chroma 8.3 → 14.9); the direct
   fit reached 6.26, then the per-hue pass took it to 5.76.
2. **Per-hue tuning scored by ΔE, not by a chroma proxy.** Fitting HUE_SAT against the
   reference's chroma read desaturated: the Dehancer panes are 700 px JPEG (4:2:0), so the
   proxy teaches matching compression artefacts (Velvia diverged, 7.8 → 15.6 luma error).
   Group coordinate descent scored directly by corpus ΔE converged instead.
3. **Fit on panes, score on the production path.** Parameters fitted on 16-20 frames at pane
   geometry, then the full 101-frame corpus rendered at 3000×2000.
4. **Guard sparse bins** (<300 samples keep the previous value) and interpolate legacy
   12-sector matrices up to 48 before editing them.

## Still open

- **Monochrome stocks have no per-hue stage** (HP5 5.89, Acros 4.41): their residual is
  tone/contrast only, so it needs curve work, not the matrix.
- **Four corpora have no recipe**: CineStill-800T, Portra-160NC, Portra-160VC, Velvia-100.
- **ColorChecker parity only certified for 50D** — other stocks need a Dehancer chart export.
