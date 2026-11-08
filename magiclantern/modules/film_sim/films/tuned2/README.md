# Colour-fitted stocks

Stage two of the fit. `films/tuned/` matched the tone response; these add the colour controls — global
saturation, twelve hue-sector gains and the six split-tone offsets — fitted by coordinate descent against
the same reference renders, re-rendering through the real engine for every candidate.

Per-hue / split / halation / grain fields start from the shipped recipe, so nothing here is invented:
each number is either carried over or moved because the measurement said so.

## Results

Mean over the 20 stocks with a counterpart, on the same photograph and the same CIEDE2000 patch statistic:

| stage | mean dE00 |
|---|---:|
| shipped recipes | 13.12 |
| + tone fit (`tuned/`) | 6.94 |
| + colour fit (`tuned2/`) | **5.50** |

Per stock:

| recipe | stock | shipped | tone | colour |
|---|---|---:|---:|---:|
| 400H | Fujicolor Pro 400H | 16.88 | 5.83 | **3.62** |
| eterna | Fuji Eterna Vivid 500 | 15.26 | 6.07 | **3.81** |
| CP200 | Kodak Color Plus 200 | 16.65 | 6.89 | **3.85** |
| P800 | Kodak Portra 800 | 19.49 | 5.36 | **3.90** |
| KCHR64 | Kodak Kodachrome 64 | 15.55 | 7.63 | **4.26** |
| realaace | Fuji Reala 500D | 7.76 | 6.65 | **4.53** |
| P160 | Kodak Portra 160 | 18.63 | 5.17 | **4.55** |
| E100 | Kodak Ektachrome E100 | 8.09 | 6.22 | **5.05** |
| C200 | Fujicolor C200 | 19.19 | 6.52 | **5.18** |
| portra | Kodak Portra 400 | 15.66 | 6.24 | **5.48** |
| 50D | Cinestill 50D | 9.84 | 6.78 | **5.87** |
| UMAX | Kodak Ultramax 400 | 18.82 | 7.63 | **6.14** |
| HP5 | Ilford HP5 Plus 400 | 6.69 | 6.69 | **6.39** |
| velvia | Fujichrome Velvia 50 | 8.39 | 6.36 | **6.35** |
| EK100 | Kodak Ektar 100 | 12.08 | 8.20 | **6.47** |
| gold | Kodak Gold 200 | 19.54 | 10.40 | **8.18** |
| soup800 | Cinestill 800T | 11.30 | 10.64 | **8.64** |
| provia | Fujichrome Provia 100F | 9.87 | 7.13 | **7.11** |
| XP2 | Ilford XP2 Super 400 | 6.19 | 5.20 | **5.20** |

## What is still left, and the way past it

Five and a half is close to the limit of what this recipe format can express: per-channel 1-D curves, a
global saturation, twelve hue gains, split tone, halation and grain. A stock whose character is a 3-D
colour transform cannot be reached by fitting those.

The engine already has a 3-D LUT slot, and the domain is sRGB-encoded RGB — the same domain a .cube is
authored in. Fitting a 17^3 LUT from the frame to the reference (a degree-3 polynomial colour transform,
sampled on the grid, packed with `tools/cube2bin.py`) measures **1.05 dE00**, and **1.13** on a held-out
half of the frame, so it is not memorising the picture. That is the route to "nearer", and it is the same
route the `.xml` film entries in this repo already point at.

One bug had to be fixed to get there: `se_cam.c` held the LUT path in a 64-byte buffer, so any path
longer than that was silently truncated and the LUT was skipped with only a "not on the card" log line.
It is 512 bytes now.
