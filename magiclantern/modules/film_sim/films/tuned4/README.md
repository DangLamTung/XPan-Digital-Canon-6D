# Luma-weighted matrix pairs

The last stage of the fit, and the closest this hardware can get without a 3-D LUT.

## What changed

A single matrix carries the linear part of the colour crosstalk. A film's crosstalk changes with density,
so the engine now takes a second matrix at white (`MATRIX_HI`) and interpolates between the two by luma:

    m(rgb) = MATRIX·rgb·(1 - t) + MATRIX_HI·rgb·t,   t = luma/255

Eighteen multiplies, one lerp, no lattice. The stage is skipped entirely unless a recipe sets it.

## Results

| stage | mean dE00 | cost |
|---|---:|---|
| shipped recipes | 13.12 | - |
| + tone fit | 6.94 | none |
| + colour fit | 5.50 | none |
| + one matrix | 3.94 | +6.1 % |
| **+ matrix pair (this)** | **2.83** | **+7.0 %** |
| 3-D LUT (not usable on the camera) | 1.04 | too slow for the body |

Measured cost: 98.1 ms/run without the matrices, 105.0 ms/run with the pair, same image and recipe, 8
runs each.

Per stock:

| recipe | stock | colour fit | + pair |
|---|---|---:|---:|
| provia | Fujichrome Provia 100F | 7.11 | **1.16** |
| velvia | Fujichrome Velvia 50 | 6.35 | **1.28** |
| portra | Kodak Portra 400 | 5.48 | **1.46** |
| 400H | Fujicolor Pro 400H | 3.62 | **1.47** |
| KCHR64 | Kodak Kodachrome 64 | 4.26 | **1.56** |
| E100 | Kodak Ektachrome E100 | 5.05 | **1.71** |
| 50D | Cinestill 50D | 5.87 | **1.75** |
| P800 | Kodak Portra 800 | 3.90 | **1.91** |
| P160 | Kodak Portra 160 | 4.55 | **1.94** |
| CP200 | Kodak Color Plus 200 | 3.85 | **2.09** |
| eterna | Fuji Eterna Vivid 500 | 3.81 | **2.19** |
| realaace | Fuji Reala 500D | 4.53 | **2.66** |
| C200 | Fujicolor C200 | 5.18 | **2.68** |
| EK100 | Kodak Ektar 100 | 6.47 | **2.71** |
| UMAX | Kodak Ultramax 400 | 6.14 | **3.21** |
| XP2 | Ilford XP2 Super 400 | 5.20 | **3.94** |
| gold | Kodak Gold 200 | 8.18 | **4.88** |
| acros | Fujifilm Neopan Acros 100 | 6.53 | **5.31** |
| soup800 | Cinestill 800T | 8.64 | **6.36** |
| HP5 | Ilford HP5 Plus 400 | 6.39 | 6.39 (no gain, not installed) |

Thirteen of twenty now sit within 2 dE00 of the reference — below the ~2.5 that is usually called a
just-noticeable difference on a patch average.

## Where the rest is

Gold 200, Acros, Cinestill 800T and Ultramax are the stragglers. Gold and 800T are stocks whose
character is strongly non-linear in chroma (the LUT reaches 0.75 and 1.28 on them), and Acros is a
monochrome stock where a matrix can only add a tint. Closing those would take a third band (27
coefficients) or the lattice itself.
