# Night autofocus for astrophotography -- design draft, not implemented

Goal: a focus routine that works when Canon's LiveView AF does not, on a 6D, at
night, on a tripod, pointed at stars. Everything here is a design. No code exists
yet.

Why Canon's AF is not enough: in LiveView the 6D focuses by contrast on the
display image. At night that image is mostly noise, the contrast peak is shallow,
and the lens hunts past it. The phase-difference sensor cannot help either -- it
needs the mirror down, which is not LiveView.

ML has the two things this needs: the LiveView frame is readable in software, and
the lens can be stepped deterministically (`lens_focus(n, stepsize, wait, delay)`,
which writes `PROP_LV_LENS_DRIVE_REMOTE` `0x80050013`).

## Two modes, different precision

### Mode A -- star sharpness, no mask

Aim the camera at a bright star, put a small ROI around it, and maximise how
concentrated the star is.

Metric, per candidate lens position, over an ROI of W x H pixels:

```
bg   = median of the ROI          (robust to a star occupying few pixels)
peak = max(v) - bg
flux = sum(max(v - bg - k*sigma, 0))     over the ROI, k ~ 3
sharp= flux / (number of pixels above threshold)
score = peak * sharp                (high when a small, bright core sits on black)
```

Rationale: defocused stars spread out, so `peak` drops and the number of above-
threshold pixels rises; `sharp` is the ratio that punishes both. Using a median
background keeps moonlight and skyglow from dominating.

### Mode B -- Bahtinov mask, spike centring

A Bahtinov mask turns a star into three spikes; when focus is exact the middle
spike sits exactly between the outer two. This is far more precise than any
sharpness metric because the geometry is amplified by the mask.

Metric: take the ROI around the star, project it onto the axis perpendicular to
the spikes, find the three peak positions `p1 < p2 < p3`, and drive the lens until
`(p2 - p1) == (p3 - p2)`. The score to maximise is `-abs((p2-p1) - (p3-p2))`.

Sub-pixel centring comes from fitting the three peaks; that gives a focus criterion
that changes measurably for a single-lens step, which Mode A often cannot.

## Search strategy (both modes)

1. **Coarse sweep.** From the current position, step `+3` up to `N_coarse` times,
   recording the score at every position. `N_coarse` default 12, i.e. +-36 steps
   of range. This is what Canon's AF does badly: it commits early, this does not.
2. **Pick the best position** by score, not by the first improvement.
3. **Fine sweep** around it: steps of `+1` over +-3 positions.
4. **Parabolic fit** through the best three points to estimate the sub-step
   optimum, then command the remainder in single steps (the lens has no sub-step
   command).
5. **Verify**: re-measure at the chosen position; if the score is below the value
   measured during the sweep by more than 20%, the frame was bad (cloud, vibration)
   and the routine repeats once from step 1.

## Frames, noise, timing

Each lens step needs the mirror-less pipeline to deliver a fresh frame before it
is measured: one `lens_focus(..., wait=1, ...)` plus **two** frame waits. At the
default LV rate that is roughly 120-200 ms per position, so a 25-position coarse
sweep is about 3-5 s. Numbers to be measured, not assumed.

Noise control:

* take **2 frames** per position and use the pixel-wise maximum, or the median of
  three, before computing the score;
* require `peak > 6 * sigma` of the background, otherwise report "no star" and
  stop instead of driving the lens on noise;
* keep the ROI small (32 x 32 is enough for one star) so the median is cheap.

## What can go wrong, and what the draft does about it

| failure | mitigation |
|---|---|
| clouds / passing haze | score verification step; retry once, then abort and leave focus where it was |
| star drifts out of the ROI (bad polar alignment) | re-centre the ROI on the brightest pixel each iteration, and cap the drift per step |
| lens backlash | always approach the final position from the same direction (the sweep already does) |
| focus drift with temperature | a "hold" mode that re-runs Mode B every N minutes when the score degrades |
| skyglow / moonlight | median background subtraction; score is relative, not absolute |
| no Bahtinov mask | Mode A only; expect +-2-3 steps of error, which at f/2.8 on a 50 mm may still show |

## What has to be measured on the camera before this is worth building

1. Does the LV image delivered to ML at night carry enough star signal, or is the
   display gain so low that no metric can work? If not, the whole draft needs a
   different exposure path.
2. How many lens steps correspond to the depth of field at the working aperture?
   That sets the sweep range.
3. The real per-position time, which decides whether a 25-position sweep is
   acceptable.

## Open questions this draft does not answer

* whether ML can raise LiveView gain/brightness for the measurement frame;
* whether the 6D's LV image is the full sensor downscaled or a crop, which changes
  how many pixels a star occupies;
* whether the lens reports any position feedback ML could read (it cannot read
  properties, so probably not).
