# Matrix-fitted stocks

The third and last stage of the fit. No 3-D LUT: this is nine multiplies per pixel.

## Why a matrix

A film LUT's advantage is *crosstalk* — the R output depends on G and B, not only on R. Per-channel curves
cannot say that, and neither can a global saturation or twelve hue gains, which is why the colour-fitted
recipes plateaued at 5.5 dE00. The linear part of that crosstalk is a 3x3 matrix, and nine multiplies is
what the camera can afford. The engine now has a `MATRIX` stage (format version 4, 8.8 fixed point, 256 =
identity), applied to the curves' output before the per-hue tables, and skipped entirely when a stock does
not set it.

This is the standard LUT-free decomposition: a matrix-TRC model (per-channel tone response curves plus a
3x3 matrix) is what ICC profiles use for exactly this reason — see [ICC matrix-TRC
profiles](https://www.color.org/groups/medical/munich2016/MIWG-Munich2016.pdf) and the discussion of what a
LUT buys over a matrix in [Light Illusion's LUT notes](https://lightillusion.com/what_are_luts.html).

## Results

| stage | mean dE00 | cost |
|---|---:|---|
| shipped recipes | 13.12 | - |
| + tone fit (`tuned/`) | 6.94 | none (same lookups) |
| + colour fit (`tuned2/`) | 5.50 | none (same lookups) |
| **+ matrix (`tuned3/`)** | **3.94** | **+6.1 % total runtime** |
| 3-D LUT (not usable on the camera) | 1.04 | lattice lookup, too slow for the body |

Measured cost: 95.6 ms/run without the matrix, 101.4 ms/run with it, same image, same recipe, 8 runs each
— 6.1 %, well inside the 50 % budget.

Per stock, the matrix stage:

| recipe | stock | colour fit | + matrix |
|---|---|---:|---:|
| KCHR64 | Kodak Kodachrome 64 | 4.26 | **1.67** |
| E100 | Kodak Ektachrome E100 | 5.05 | **2.25** |
| 400H | Fujicolor Pro 400H | 3.62 | **2.42** |
| portra | Kodak Portra 400 | 5.48 | **2.46** |
| CP200 | Kodak Color Plus 200 | 3.85 | **2.48** |
| 50D | Cinestill 50D | 5.87 | **2.69** |
| P160 | Kodak Portra 160 | 4.55 | **2.98** |
| P800 | Kodak Portra 800 | 3.90 | **3.02** |
| provia | Fujichrome Provia 100F | 7.11 | **3.42** |
| eterna | Fuji Eterna Vivid 500 | 3.81 | **3.45** |
| velvia | Fujichrome Velvia 50 | 6.35 | **3.64** |
| realaace | Fuji Reala 500D | 4.53 | **3.71** |
| EK100 | Kodak Ektar 100 | 6.47 | **3.94** |
| C200 | Fujicolor C200 | 5.18 | **4.06** |
| acros | Fujifilm Neopan Acros 100 | 6.53 | **5.26** |
| UMAX | Kodak Ultramax 400 | 6.14 | **5.27** |
| gold | Kodak Gold 200 | 8.18 | **5.93** |
| HP5 | Ilford HP5 Plus 400 | 6.39 | 6.39 (no gain, not installed) |
| XP2 | Ilford XP2 Super 400 | 5.20 | 5.20 (no gain, not installed) |
| soup800 | Cinestill 800T | 8.64 | 8.64 (no gain, not installed) |

Three stocks gain nothing: two are monochrome (a matrix on a neutral image can only tint it) and
Cinestill 800T's residual is not linear. Their `tuned3` files carry no matrix, so they render exactly as
their `tuned2` versions do — a fitted matrix that does not help is not installed.

## What this means for the remaining gap

3.94 against a 1.04 LUT floor. The matrix took the linear part of the crosstalk; what is left is its
non-linear part — the part a lattice interpolates. Closing that without a lattice would need more than
nine coefficients, e.g. two or three matrices selected by luma (3x9 = 27 coefficients, still ~20
multiplies), which is the obvious next step if the budget allows it.
