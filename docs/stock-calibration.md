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

| Stock | Baseline | Verified | Median | ≤4.0 | Blown % (ours / Dehancer) |
|---|---|---|---|---|---|
| Kodachrome 64 | 11.76 | 4.84 | 4.81 | 13/101 | 0.23 / 3.87 |
| Velvia 50 | 8.31 | 4.83 | 4.74 | 25/101 | 0.13 / 0.30 |
| Portra 160 | 6.94 | 4.46 | 4.42 | 33/101 | 6.73 / 6.84 |
| Ektar 100 | 10.95 | 4.40 | 4.29 | 41/101 | 0.07 / 2.81 |
| Cinestill 50D | 4.51 | 4.14 | 4.11 | 45/101 | 0.08 / 2.57 |
| Portra 160NC | new | 4.09 | 4.07 | 49/101 | 9.06 / 8.71 |
| CineStill 800T | new | 4.08 | 4.05 | 48/101 | 0.05 / 0.43 |
| Velvia 100 | new | 4.05 | 3.96 | 55/101 | 0.58 / 3.00 |
| Pro 400H | 12.00 | 4.01 | 4.04 | 49/101 | 0.15 / 3.79 |
| Portra 160VC | new | 3.76 | 3.64 | 67/101 | 6.81 / 5.96 |
| Portra 800 | 12.73 | 3.68 | 3.69 | 73/101 | 0.02 / 2.03 |
| Portra 400 | 11.97 | 3.67 | 3.67 | 69/101 | 0.02 / 0.01 |
| Ektachrome E100 | 6.48 | 3.62 | 3.59 | 70/101 | 0.01 / 0.62 |
| Gold 200 | 12.50 | 3.62 | 3.65 | 74/101 | 0.00 / 0.03 |
| ColorPlus 200 | 13.69 | 3.55 | 3.56 | 80/101 | 0.05 / 2.03 |
| HP5 Plus | 6.97 | 3.46 | 3.46 | 86/101 | 1.68 / 3.75 |
| Provia 100F | 9.23 | 2.96 | 2.87 | 95/101 | 0.00 / 0.04 |
| Acros 100 | 6.62 | 2.25 | 2.24 | 101/101 | 0.05 / 2.49 |
| XP2 Super | 4.38 | 1.78 | 1.66 | 100/101 | 0.02 / 0.73 |

Every verified corpus improved. Largest moves: Portra 800 12.73 → 3.66,
ColorPlus 200 13.69 → 3.94, Gold 200 12.50 → 3.52, Pro 400H 12.00 → 3.97,
XP2 Super 4.38 → 1.78 (100/101 ≤4.0), Provia 100F 9.23 → 2.96.

## Method that worked

1. **Feed-forward tone fit, not residual.** The engine applies the master curve before the
   per-channel curves, so `curve_c` must be fitted on `master(source_c)`. A residual fit
   drifts and lifts blacks (Kodachrome 11.76 → 11.65, neutral chroma 8.3 → 14.9).
2. **Per-hue tuning scored by ΔE, not by a chroma proxy.** Fitting HUE_SAT against the
   reference's chroma reads desaturated: the Dehancer panes are 700 px JPEG (4:2:0), so the
   proxy teaches matching compression artefacts.
3. **Monochrome stocks need their tone.** Dehancer tones its B&W (HP5 a* −4.2/b* +2.7,
   Acros a* −1.3/b* +3.8); split tinting took HP5 5.89 → 3.43 and Acros 4.41 → 2.23.
4. **Sector saturation can carry a cast.** Velvia's skin read +4.0 a* magenta with every
   global control neutral; a 15 % cut on sectors 0-8 fixed the skin and the midtones.
5. **Fit on panes, score on the production path.** Parameters fitted on 16-20 frames at pane
   geometry, then the full 101-frame corpus rendered at 3000×2000.

## Still open

- **Ektar 100 / Ektachrome E100**: the calibrated versions measure closer to Dehancer
  (4.93 / 4.56 vs 10.95 / 6.48) but the owner prefers the original look; decision pending.
- **Kodachrome 64** (5.75) is the last stock above 5. Its residual is saturated-blue and
  green-hue behaviour, not tone.
- **Portra 160NC/VC** clip above their reference (10.5/8.3 % vs 8.7/6.0 %).
- **ColorChecker parity** is only certified for 50D; other stocks need a Dehancer chart export.
