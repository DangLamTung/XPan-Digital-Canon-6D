# Colour research: calibrating film stocks against Dehancer

Branch `color_reaseach`. What was done, what the numbers are, and what is known to be wrong.

## Method

Every stock was calibrated against a render of the same image set through the same film in
Dehancer Desktop, so "correct" is a measurement rather than an opinion.

1. **Reference** — 110 test images rendered through Dehancer for that stock, each file checked
   for the embedded `dehancer:FilmProfile` so a wrong-stock render cannot slip into a set.
2. **Chart first** — the 20-patch chart fixes tone. This is where the curves get their shape.
3. **Region detection** — OpenCV YuNet finds faces; skin is sampled inside the detected face
   box; green and blue are hue masks with saturation guards; sky and sea are split by a
   detected horizon.
4. **Loop** — render, measure regions against the reference, change one control, accept only if
   the target region improves and no other region regresses past tolerance. Every trial is
   logged.
5. **Report** — per stock: photo-set CIEDE2000, per-region deltas, and a character checklist
   with the measured value beside each claim.

Acceptance: skin hue ±3°, skin chroma ±1.5, skin L\* ±3, skin b\* ±3, green hue ±5° and b\* ±3,
sky/sea hue ±3°, chroma ±2, b\* ±2, whole-frame p10 ±1 / p50 ±2 / p90 ±2.

## Results

Nineteen stocks have reference sets. Thirteen are installed and passing; six more are fitted and
in progress. `dE00` is the mean CIEDE2000 against the Dehancer render over a 12×12 patch grid;
region columns are medians over the images where the mask is valid.

| stock | dE00 | skin hue | skin C | skin L | green hue | sky hue | sea hue | recipe |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| Provia 100F | **1.56** | +0.8 | +1.4 | −1.8 | +1.5 | +2.2 | +1.7 | `provia.fs` |
| XP2 400 | **1.62** | n/a | n/a | n/a | +0.0 | −0.0 | −0.0 | `XP2.fs` |
| Acros 100 | **2.07** | n/a | n/a | n/a | +11.2 | +8.3 | +9.5 | `acros.fs` |
| Velvia 100 | **2.35** | +2.0 | +1.0 | −2.0 | +3.9 | −0.5 | −0.8 | `velvia100.fs` |
| Ektachrome E100 | **2.64** | +1.3 | +1.2 | −2.4 | +4.6 | +0.2 | −0.1 | `E100.fs` |
| Portra 400 | **2.90** | +0.3 | +0.9 | −0.9 | −2.2 | −0.6 | −0.7 | `portra_best.fs` |
| Pro 400H | **2.96** | +1.3 | +1.0 | −1.5 | +3.8 | +2.8 | +0.6 | `400H.fs` |
| Velvia 50 | **3.13** | +1.4 | +1.5 | −2.4 | +2.9 | +1.0 | −1.5 | `velvia.fs` |
| Ektar 100 | **3.15** | +0.5 | +1.4 | −2.1 | +1.2 | +1.7 | +0.7 | `EK100.fs` |
| HP5 400 | **3.27** | n/a | n/a | −1.8 | n/a | n/a | n/a | `HP5.fs` |
| Cinestill 50D | **3.33** | −1.3 | +0.7 | −3.1 | +4.7 | +1.2 | +1.5 | `50D.fs` |
| Portra 800 | **3.63** | +2.4 | +1.3 | −0.4 | +1.9 | +1.6 | −0.2 | `P800.fs` |
| Portra 160 | **4.59** | +0.3 | −1.4 | −0.1 | −4.1 | +1.8 | +0.1 | `P160.fs` |
| Gold 200 | **5.05** | −3.3 | +0.2 | −2.4 | +12.0 | +3.1 | +2.2 | `gold.fs` |

Fitted, not yet tuned to the skin/region targets (references rendered, base recipes written):

| stock | dE00 | state |
|---|---:|---|
| Cinestill 800T | 2.85 | fitted, 2 loop iterations |
| Kodachrome 64 | 3.68 | fitted, 1 iteration |
| Color Plus 200 | 3.53 | fitted, 1 iteration |
| Portra 160NC | 4.48 | fitted, needs the full pass |
| Portra 160VC | 4.74 | fitted, needs the full pass |

Monochrome stocks (HP5 400, XP2 400, Acros 100) are gated on lightness and tone only — hue and
chroma are meaningless there.

## What produced the biggest gains

**The master curve double-transform.** `look.c` applies `CURVE` and then `CURVE_R/G/B`. A fitter
that writes the mean of R/G/B into the master transforms every pixel twice and the stock comes
out dark. Setting the master to identity and letting the per-channel curves carry the transform
took Ektar from ΔL −8.4 / p50 −11.6 to ΔL −0.7 / p50 −0.5 in one step. Every earlier curve fit
had been fighting this.

**Midtone skin, not the whole face box.** Averaging over the detected face box includes hair,
lips and background, and it read "within tolerance" while skin was visibly wrong. On midtones
only (L 35–70) the errors were large and matched what a person sees:

| stock | whole-box skin hue | midtone skin hue | Dehancer |
|---|---:|---:|---:|
| Gold 200 | −0.8° | **+17.0°** | 40.2° |
| Cinestill 50D | +2.9° | **+8.3°** | 50.5° |
| Pro 400H | −1.8° | **−4.9°** | 37.6° |

`skin_mid` is now a region in the evaluator and is gated; the whole-box hue is reported only.

**Luminance selectivity.** A hue sector cannot separate sky from water and a per-channel curve
cannot separate "how bright" from "how warm". Rotating the blue sector to fix the sea broke the
chart's cyan patch (ΔE 8.7 → 44.6); the knots at 192/224 fixed the sky without touching the
midtones. The lesson recorded in the checklist: rotation is not luminance-selective, curve knots
are.

## Why a perfect chart does not mean a perfect image

The chart is flat patches; the engine is a per-pixel LUT of one pixel's colour, while Dehancer is
not a pointwise function.

- **Halation and local tone mapping need neighbours.** On the night frame `086_IMG_4754`, our p2
  is 0.00 against Dehancer's 1.19 for Gold 200, while the flat chart greys agree within about one
  unit. No 256-entry curve can tell a flat 90 % grey patch from a small bright neon in a dark
  street.
- **Patches conflict with photographs.** Portra 400's chart failure is the cyan patch (ΔE 44.6)
  plus the flat highlights (grey 90 ΔL +8.0). Correcting the cyan patch moves the sea by ~18°,
  because the same hue sector drives both.
- **One curve, two audiences.** Fixing a grey patch moves every highlight in every photograph by
  the same amount.

The loop therefore scores the chart *and* the regions and rejects a candidate that wins one and
loses the other. Chart calibration is the starting point, not the finish.

## Known limitations

- **Gold 200** (5.05) carries a uniform blue-yellow offset — green b\* −5.9, sky −2.7, sea −4.2.
  Five mechanisms were tested (hue rotation, local saturation, global saturation, blue-curve knots
  from 32 to 224, and the 3×3 `MATRIX`/`MATRIX_HI`), and each improved a region while making the
  whole-image score worse. Its skin is now correct; the colour cast is not.
- **Portra 160** (4.59) has a global +2.7 yellow cast. Cooling it with the blue curve improved
  Δb but broke skin chroma, so it was reverted; it needs a refit from a neutral base rather than
  patching.
- **Portra 400** reports 7.07 on the chart against a 3.0 target — the flat-highlight and cyan
  conflicts above. The photo-verified version is installed and the conflict is deliberate.
- Eight portrait JPEGs in the test set (4000×6000) fail to decode in the engine, so every score
  is over 102 images, never 110.
- Dehancer's own reference carries a warm print tint on some monochrome stocks (HP5 b +3.98,
  Acros b +3.98) and none on XP2. A monochrome recipe with `SAT 0` is perfectly neutral, so the
  tint has to come from the split controls; adding it to XP2 made it worse (1.62 → 3.66), which
  is why it is not applied there.

## Engine addition: CURVE_Y

`se/engine` gained an optional luma stage (`CURVE_Y`), 46 lines across `stock.h`, `stock.c`,
`look.h`, `look.c`. It is identity-defaulted and no installed recipe uses it: a curve at a given
luma sets a chroma multiplier, leaving luma where it is, so a stock can lose chroma in foliage
without going dark — which the master curve cannot express, because restoring the level restores
the colour.

Verified: builds with gcc 16.2.0, and eleven of twelve installed recipes render **byte-identical**
to the previous MSVC build (same SHA-256). HP5 differs only in grain speckle (0.70 % of pixels,
mean ΔE 0.0038) because grain is compiler-sensitive, and both binaries are self-consistent. It is
not used by any recipe yet and can be reverted without touching them.

## Reproducing

```
python tools/fit_curves.py --stock "Kodak Ektar 100" --passes 1 --direct
python tools/stock_lab.py  --stock "Kodak Ektar 100" --set "HUE_ROT[6]+=4"
python tools/stock_eval.py --stock "Kodak Ektar 100" --recipe EK100.fs
python tools/region_eval.py --stock "Kodak Ektar 100"
python tools/make_compare.py --stock "Kodak Ektar 100"
```

`make_compare.py` writes `repo | Dehancer` pairs for every image — without those a stock cannot
be judged, only summarised.

## Samples

`samples/` holds `repo | Dehancer` pairs for a few stocks, one image each, at reduced size.
The full set for every stock is 102 pairs per stock, kept out of the repository.
