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

## Classic Negative is Superia, and the numbers say so

Fujifilm's *Classic Negative* simulation reproduces **Fujicolor Superia**, the
consumer colour negative Fuji launched in 1998; the look is Superia pushed --
harder contrast, cyan-green shadows, greens that read olive.

This repository already carries Superia in two forms:

* **recipes**: `films/SUP200.fs` (Superia 200), `films/SUP400.fs`, `films/SUPRA.fs`
* **ground truth**: `gt2048/Fujicolor_Superia_200_+_3513/` -- 102 Dehancer
  references plus a chart, matching the `batch110b/` source frames by name

So Classic Negative can be measured against a real reference instead of judged by
eye. On frame `001_IMG_6883.jpg`, median CIEDE2000 against the Dehancer Superia
200 reference:

| recipe | median dE00 |
|---|---|
| `SUP200.fs` (our Superia 200) | **3.07** |
| `classicneg.fs` (our Classic Neg) | **9.43** |

`classicneg.fs` is not a Superia recipe that is slightly off; it is a different,
much bluer and harder rendering, and it was never calibrated (it has no entry in
the standings above and no corpus of its own). Three times the error of `SUP200`
on the same frame is the measurement behind "it looks wrong".

The honest options, in order:

1. use `SUP200.fs` when the intent is Classic Negative -- it is measured, and it
   is the film the simulation is named after;
2. rebuild `classicneg.fs` as `SUP200` plus a controlled push (contrast up, shadow
   split toward cyan), then measure it on this corpus until the number drops;
3. keep it out of calibrated comparisons until (2) is done.

The comparison image is `renders/dalat-calib/compare-superia-vs-classicneg.jpg`.

## Classic Negative measured against Fuji's own examples

There is no Dehancer corpus for Classic Negative, so the look was measured against
Fuji's published examples instead: the three-photo collage on Fuji's IRODORI
article about クラシックネガ (saved as `docs/img/cn-reference-fuji.jpg`).

Signature, measured as shadow mean (darkest 20%), highlight mean (brightest 10%),
saturation and the 1st-percentile black level:

| sample | shadow RGB | highlight RGB | sat | black |
|---|---|---|---|---|
| Fuji panel 1 (snow, umbrella) | 79, 99, 117 | 234, 237, 243 | 0.16 | **49.8** |
| Fuji panel 2 (pond reflection) | 17, 37, 38 | 147, 170, 186 | 0.48 | **26.6** |
| Fuji panel 3 (cat, red cup) | 36, 44, 41 | 217, 217, 220 | 0.11 | **19.7** |
| our `classicneg.fs`, frame 01 | 10, 36, 62 | **202, 179, 136** | **0.50** | 19.2 |
| our `classicneg.fs`, frame 07 | 6, 23, 34 | **221, 182, 109** | **0.50** | **9.7** |
| our `classicneg.fs`, frame 10 | 16, 34, 52 | 254, 251, 245 | 0.26 | **9.3** |

Two differences are not scene-dependent:

* **blacks**: Fuji's examples sit at 20-50, ours at 9-19. `CURVE 0,0` keeps an
  absolute black that Classic Negative does not have;
* **saturation**: Fuji's (0.11-0.48, panel 2 is a reflection so it runs high),
  ours flat at 0.50.

The third difference -- warm highlights (ours R >> B, Fuji's B >= R) -- is partly
scene: a golden-hour pine grove cannot have neutral highlights. It is still the
direction the split tone should take, and `SPLIT_HI 30,10,-22` points the other
way from a cool highlight.

`samples/classicneg-v4.fs` is a candidate with `CURVE 0,14 ...`, `SPLIT_HI -7,2,11`
and `SAT 98`; on the same three frames it reads:

| | shadow | highlight | sat | black |
|---|---|---|---|---|
| v4 frame 01 | 22, 46, 70 | 190, 177, 151 | 0.40 | 31.8 |
| v4 frame 07 | 15, 35, 48 | 213, 180, 118 | 0.42 | 21.9 |
| v4 frame 10 | 26, 45, 64 | 242, 246, 250 | 0.22 | 23.1 |

Blacks and saturation move toward Fuji's numbers. It is still **not a verified
stock**: this compares a signature, not pixels of the same scene. It becomes
verified the day a Classic Negative reference of *our* frames exists (Dehancer
export of the same corpus), which is what the 19 other stocks have.

Visual: `renders/classicneg-vs-reference.jpg`.

### Round two: hue and lightness against the same reference

Measured in L*a*b*, hue angle of the shadow (darkest 20%) and highlight
(brightest 10%) zones, their chroma, and the black level:

| sample | shadow hue | shadow chroma | highlight hue | black L* |
|---|---|---|---|---|
| Fuji panel 1 (snow) | -108.5 | 12.2 | -88.9 | 32.7 |
| Fuji panel 2 (pond) | -158.8 | 8.4 | -35.8 | 27.7 |
| Fuji panel 3 (cat) | -159.8 | 0.4 | -66.0 | 0.0 |
| current `classicneg.fs`, f10 | -91.6 | 13.6 | **+92.3** | **9.3** |
| v4, f10 | -92.2 | 14.4 | -109.1 | 23.1 |
| **v5, f10** | **-127.4** | **8.9** | -109.3 | **28.9** |
| **v5, f01** | **-115.5** | **11.9** | +89.4 | 36.6 |
| **v5, f07** | **-160.9** | 8.7 | +81.0 | 27.3 |

`samples/classicneg-v5.fs` (v4 plus `SPLIT_SH -18,16,26`) is the first version
whose measured signature lands inside Fuji's own range on the parts that are not
scene-dependent:

* shadow hue -115 to -161, against Fuji's -108 to -160 -- cyan-green, not blue;
  the current recipe sits at -92, i.e. pure blue;
* black L* 27-37, against Fuji's 28-33;
* highlight hue and chroma follow the scene (a golden pine grove stays warm, as
  Fuji's cat-with-red-cup panel stays warm).

That is still a **signature match on a different scene**, not a calibrated stock.
v5 is a candidate, kept in `samples/` for exactly that reason. Visual:
`renders/classicneg-v5-vs-reference.jpg`.

## The field-house frame: which stock, and what we can take from it

`samples/reference/field-house.jpg` (2048x1366, no EXIF) is a daylight frame --
green field, brick house, cumulus sky -- with the lifted blacks and cyan sky of a
consumer colour negative.

Measured in L*a*b*: shadow (darkest 20%) hue **140.6°**, chroma 23.4, L* 28.1;
highlight hue 57.7°, chroma 7.0, L* 86.3; saturation 0.404; **black L* 34.0**;
white L* 227.6. Patches: clouds (168,181,192) -- blue, not warm; sky
(133,184,213); brick (130,89,58); grass (86,124,73).

**Which film.** The combination of *cool* highlights (clouds blue), green-cyan
shadows, punchy greens and lifted blacks is Fuji's consumer negative family --
**Superia 200/400**. It is the same film Classic Negative is modelled on, and this
repository already carries it: `films/SUP200.fs`, measured at median dE00 3.33
over 102 frames against Dehancer's Superia 200. Kodak's Gold/Portra go the other
way (warm highlights, magenta skies), so they are the wrong family here.

**What we took from it.** One trait the reference has and `SUP200.fs` does not:
the blacks sit at L* 34, ours at 9-11. `samples/superia-lifted.fs` is SUP200 with
`CURVE 0,18 ...`, a cooler highlight split (`SPLIT_HI 0,6,10`) and a greener
shadow (`SPLIT_SH -4,10,8`). Measured on the Da Lat frames:

| | black L* | saturation |
|---|---|---|
| reference | 34.0 | 0.404 |
| `SUP200.fs` f07 | 9.4 | 0.417 |
| `superia-lifted.fs` f07 | **32.4** | 0.287 |
| `superia-lifted.fs` f10 | **33.4** | 0.13 |

The black lift lands. Saturation is *not* matched and should not be chased
numerically: the reference's 0.404 comes from a frame that is 60% green, and
raising `SAT` to reproduce that scalar on our flower frame turned the yellow
flowers orange. One scalar from one scene is not a calibration.

So: `superia-lifted.fs` stays in `samples/` as a candidate for the "scan-lifted
Superia" look, not as a shipped stock. Visual: `renders/fieldhouse-reference-vs-ours.jpg`.
