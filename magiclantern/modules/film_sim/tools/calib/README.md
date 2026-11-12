# ColorChecker calibration pipeline (48-knot engine)

Fits a Magic Lantern `.fs` recipe to Dehancer's render of the X-Rite ColorChecker, then to the
photographic domains, in the order that keeps the colour chart authoritative.

## Order

1. **`chart_1to1.py`** — correct the chart first. Per-knot coordinate descent with acceptance
   over the hue wheel (saturation, rotation, luminance) plus the tone curve, so a move is kept
   only if the mean CIEDE2000 over the 24 patches improves. This is the step that must be right:
   the wheel is the hue, and everything downstream inherits its errors.
2. **`calib48.py --stage chart`** — the same objective with the branch's CIEDE2000 domain
   weights, used for a second opinion on the chart.
3. **`calib48.py --stage domains`** / **`tweak48.py`** — HSL tweak on top of the corrected
   chart, with the chart held as a hard term so the domains cannot win by breaking it.
4. **`fit48.py`** — the earlier per-knot bisection fit; kept because it is the fastest way to a
   good chart starting point (7.92 -> 2.12 in two passes).

## Measurement

- `cc_compare.py` — patch extraction and CIEDE2000, the metric everything reports.
- `eval_branch.py` — scores a recipe over the 102 image evaluation set (skin shadow/mid/high,
  green, blue, red, cyan) against `E:\dehancer_final\<stock>`.
- `prepare_small.py` — upright 700 px copies of the 102 originals. The originals are 6000x4000
  with EXIF orientation 8 on the portrait frames; resizing without applying the orientation
  leaves them landscape and comparing that to Dehancer's upright 4000x6000 render produces
  region errors of ~19 dE00 that are pure misalignment.
- `gd48.py` — SPSA gradient descent over the 144 knots; kept for reference, it moved the total
  loss 14.03 -> 13.84 without changing the independent metric.

## Result on CineStill 50D (48-knot engine)

| | chart mean dE00 | chart max | photos dE00 |
|---|---:|---:|---:|
| branch `50D.fs` as pulled | 8.25 | 18.3 | 3.81 |
| `50D_chart1to1.fs` | **1.78** | **5.5** | 4.52 |

13 of 24 patches are under dE00 1.5. The residual is the grey ladder (2-5 L* too bright, which
only the tone curve can move) and light skin / red, which share a knot neighbourhood.

## Paths

The scripts assume the working layout used during calibration: `E:\chart_cc\cc.jpg` (chart
source), `chart_cc_deh\Cinestill-50D\cc.jpg` (Dehancer's render), `E:\fit_small` (upright
copies), `E:\dehancer_final\<stock>` (references) and `chart_work\eval102.txt` (the 102 image
list). They are absolute on purpose — the calibration is reproducible against those exact
references, and silently picking up a different chart is how wrong numbers get published.
