# Tuned stocks

These recipes are the shipped looks with their tone curves re-derived from a reference render, not
hand-edited guesses. Each one is a candidate: the shipped file in `films/` is unchanged, and this
directory holds the version that measured closer to the reference.

## What was measured against

Dehancer Desktop renders of the same stock on the same photograph, exported at 1913x813 through its own
Export Settings with the trial watermark absent (verified: the overlay detector scores every export at
0.04-0.06, the clean band). The comparison statistic is CIEDE2000 on a 16x16 patch grid, the same one
used for the 62-stock survey of that engine.

## Method

The grade engine's per-channel curves are monotone lookup tables in the sRGB display domain, so a target
tone response can be read out of a reference render by quantile matching:

    y(x) = target_quantile( source_quantile(x) )

Nine knots at x = 0, 32, 64, 96, 128, 160, 192, 224, 255 carry that mapping; `SAT` is then scanned
(40..180, step 10) for the best score, and the per-hue / split-tone / halation / grain fields are carried
over from the shipped recipe untouched. Black-and-white stocks (SAT 0) fit one shared luma curve instead
of three channel curves, because a per-channel fit invents a colour cast the mono path never had.

## Results

Mean over the 20 stocks that have a counterpart: **13.12 -> 6.91 dE00 (47 % closer)**.

| recipe | Dehancer stock | before | after |
|---|---|---:|---:|
| P160 | Kodak Portra 160 | 18.63 | **5.17** |
| P800 | Kodak Portra 800 | 19.49 | **5.36** |
| 400H | Fujicolor Pro 400H | 16.88 | **5.83** |
| eterna | Fuji Eterna Vivid 500 (Exp. 2013) | 15.26 | **6.07** |
| E100 | Kodak Ektachrome E100 (2017) | 8.09 | **6.22** |
| portra | Kodak Portra 400 | 15.66 | **6.24** |
| velvia | Fujichrome Velvia 50 | 8.39 | **6.36** |
| 50D | Cinestill 50D | 9.84 | **6.78** |
| CP200 | Kodak Color Plus 200 | 16.65 | **6.89** |
| provia | Fujichrome Provia 100F | 9.87 | **7.13** |
| UMAX | Kodak Ultramax 400 | 18.82 | **7.63** |
| KCHR64 | Kodak Kodachrome 64 (Experimental) | 15.55 | **7.63** |
| EK100 | Kodak Ektar 100 | 12.08 | **8.20** |
| gold | Kodak Gold 200 | 19.54 | **10.40** |
| soup800 | Cinestill 800T | 11.30 | **10.64** |
| C200 | Fujicolor C200 | 19.19 | **6.52** |
| realaace | Fuji Reala 500D (Exp.2013) | 7.76 | **6.65** |
| XP2 | Ilford XP2 Super 400 | 6.19 | **5.20** |

Two stocks were **not** kept, because the fit did not improve them and a regression is worse than no
change: `acros` (6.53 -> 7.01) and `HP5` (6.69 -> 6.79). Their shipped recipes stay as they are.

## The ceiling

Every remaining number is above 5, while the same measurement puts this project's own chain within about
3 dE00 of the reference. The gap is expressible-range: this engine has per-channel 1-D curves, a global
saturation, twelve hue sectors, split toning, halation and grain. A stock that is a 3-D film LUT plus a
print stage cannot be reached by fitting those, no matter how good the fit is — the curves can match the
tone, but not the colour crosstalk a film LUT carries.
