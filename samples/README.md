# Samples

Scenery, so a look can be judged on something a look is for: eleven frames the
author shot in Đà Lạt on the morning of 2 November 2026, on the 6D this module
runs on, plus `00-the-strip.png`, the contact sheet they were first shared as.

| file | frame | what is in it |
|---|---|---|
| `01-pine-grove-morning.jpg` | IMG_9919 | a pine grove in low sun, a deer under the trees |
| `02-hillside-pines.jpg` | IMG_9920 | hillside pines, the valley and the road below |
| `03-lookout.jpg` | IMG_9921 | two people at a lookout, pines, long shadows |
| `04-backlit-pines.jpg` | IMG_9923 | backlit pines on a slope, the sun in the frame |
| `05-granite-and-moss.jpg` | IMG_9925 | mossy granite with the sun flaring through it |
| `06-path-through-pines.jpg` | IMG_9930 | a dirt path through the pines in morning mist |
| `07-yellow-flowers.jpg` | IMG_9931 | a bank of yellow flowers, the most colour of the set |
| `08-road-in-haze.jpg` | IMG_9932 | the road out of town, haze, two scooters |
| `09-ridge-and-valley.jpg` | IMG_9934 | the ridge and the town in the valley below |
| `10-lake-and-tower.jpg` | IMG_9937 | Xuan Huong lake, the church tower behind the pines |
| `11-lake-wide.jpg` | IMG_9938 | the same lake, wider, the town on the far bank |

These are **develops of the camera's raw files**, not the camera's JPEGs, at
1600 px wide. That distinction is the whole point of this folder: those JPEGs
were shot with a look in the slot, so they carry it already. Measured against
the raw develop of the same frame, the JPEG of IMG_9923 has channel means of
55/71/85 where the develop has 94/95/91 — thirty levels of blue over red, the
teal veil you can see — and IMG_9931's JPEG reads 59/52/19 against 90/90/53. A
look judged on top of another look is a look judged twice.

So each one is `rawdev` — the module's own level-3 path — run with
[`base-neutral.fs`](base-neutral.fs): identity curves, no hue move, no split
tone, no grain, and `EXPOSURE 1.8`, which is where an identity transform has to
sit to land on the brightness of the camera's own JPEG (at 0, IMG_9919 develops
to mean luma 48 against the JPEG's 74, and IMG_9932's 75 against 147).

Nothing here is a shipped stock; it is a measuring stick. The 65:24
XPan framing was set at capture, so the bar window Canon burned into its JPEG is
read off that JPEG and cropped to, leaving the XPan image itself.

Rebuild all of it from the card:

```sh
git -C . rev-parse --short HEAD   # what this run was made from
python3 magiclantern/modules/film_sim/tools/make_samples.py
```

## The look sheet

`look-sheet.jpg` is the same eleven frames through the module: in each row the
frame with no look first, then Portra, Classic Neg, Velvia and Tri-X. Every tile
was written by `se/hosttest/grade_jpeg`, which runs the camera's develop colour
path — decode → curves → twelve hue knots → split tone → halation → grain —
and the tiles are only enlarged, so grain and colour can be seen.

```sh
python3 magiclantern/modules/film_sim/tools/look_sheet.py \
    samples/dalat/look-sheet.jpg --zoom 1 --width 900 \
    --scene samples/dalat/01-pine-grove-morning.jpg "01 pine grove" \
    --scene samples/dalat/03-lookout.jpg "03 lookout" \
    --stock magiclantern/modules/film_sim/films/EK100.fs
```

With no `--stock` it uses the four above; the first column is always a stated
identity recipe, so the "no look" reference cannot drift with the stocks.
`--width` only shrinks the sheet's input — the sample files keep their size.

One frame on its own:

```sh
cd magiclantern/modules/film_sim/se/hosttest && make grade_jpeg
./grade_jpeg ../../../../../samples/dalat/03-lookout.jpg \
             ../../films/portra.fs /tmp/portra.jpg
# prints: out: 800x300, neutral 0.0%, mean 132,116,95
```

It reports what it wrote, including the share of neutral pixels, so a file that
came out grey cannot pass for a graded one.

## Calibrated-stock sheet

`tools/dalat_calib_sheet.py` renders these eleven frames through the module's own
develop colour path (`se/hosttest/grade_jpeg`, the same code the camera runs) for
every stock that `docs/stock-calibration.md` lists as verified, and writes:

```
renders/dalat-calib/<stock>/<photo>.jpg     132 renders, 11 photos x 12 stocks
renders/dalat-calib/sheet-1..3.jpg          no look, then five stocks per sheet
```

The stocks are ordered by their verified CIEDE2000 median, best first: XP2,
Acros, Provia, HP5, ColorPlus, Gold, Ektar, Portra 160, Portra 400, Velvia 50,
Pro 400H, Classic Neg.

One honest caveat, and the reason the script computes it rather than assuming:
these frames are **already developed JPEGs**, while the stocks are raw-domain
looks. Applied at exposure 0 they came out roughly a stop dark (mean 65 against
the source's 88). The script therefore measures the source mean once and picks
the exposure per stock that lands the graded mean back on it -- 0.4 to 1.2 EV
depending on the stock -- so the sheet compares colour, not brightness. Rendering
from the original CR2s would not need that step; those files are not in this
repository.
