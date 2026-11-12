# Film frame on the camera

A developed file can carry a rebate: a band top and bottom, in the stock's brand
colour, with the stock name and frame number above and the camera line below.
It is **off by default** — it changes the file the camera writes, so it is opt-in.

## Turning it on

`Modules → film_sim → Film Frame` (config key `film.sim.frame`, 0/1).

It applies to **developed files only**, not to the LiveView preview, not to the
in-camera thumbnail and not to the small preview develops — a frame belongs on
the file that is kept.

## What it draws

A print border: a plain margin, and a wider band along the bottom carrying the
camera on the left and the exposure on the right.

```
┌──────────────────────────────────────────────┐
│                the photograph                │
├──────────────────────────────────────────────┤   keyline
│ CANON                       f    SS    ISO   │
│ EOS 6D                     2.8  1/200   400  │
└──────────────────────────────────────────────┘
```

- **Left**: brand line in the main text colour, camera model under it in a
  dimmed grey — `CANON` / `EOS 6D`, the model from ML's `camera_model`.
- **Right**: three columns, each a small label (`f`, `SS`, `ISO`) centred over
  its value.
- **Values** are decoded from the same tables ML uses for its own display:
  `values_aperture`/`codes_aperture`, `values_shutter`/`codes_shutter`,
  `values_iso`/`codes_iso` against `lens_info.raw_aperture` / `raw_shutter` /
  `raw_iso`. A raw value not in a table — bulb, an adapted lens — shows `-`.
- **Paper**: white (`#FFFFFF`) with a dark keyline where the border meets the
  picture. The text colours derive from the paper's luma, so the same layout
  works if the paper colour is changed.
- **Wordmark**: the official `Canon` wordmark — sourced from Wikimedia Commons
  (`Canon_wordmark.svg`, a PD-textlogo file), rasterised to a 240x50 1-bit
  mask and drawn 1:1 in Canon red `#CC0000`. The mark is a Canon trademark and
  is used here only to label photographs the camera took; it is not part of
  this project's licence.
- **Type**: rasterised Arial, proportional and baseline-aligned, 12 px at
  scale 1 — not a 5x7 glyph grid, which reads as a camera overlay rather than
  as print. Two tables are baked into `frame.c`: the wordmark bitmap and the
  per-glyph bitmaps with their widths and advances.

## Where it lives

| file | role |
|---|---|
| `se/frame.c`, `se/frame.h` | the compositor: font, band fill, keyline, text rows. No camera dependencies, no allocation |
| `se/se_cam.c` | `write_band()` + `se_cam_set_frame()`; the develop adds `frm_top + frm_bot` rows, writes the bands around the picture |
| `film_sim.c` | the menu item, `fs_frame_apply()` (brand colour, label, camera line), called immediately before each develop |
| `se/hosttest/frameprev.c` | host preview: draws the same bands on a synthetic image and writes a PPM |

The compositor shares the module's arena discipline: one row at a time, through
the same `se_jpeg_write_rows()` the picture uses, so the file is one image and
not an image with a trick in it.

## Checking the design without a camera

```bash
cd magiclantern/modules/film_sim/se/hosttest
cc -std=c99 -O2 -o frameprev frameprev.c ../frame.c
./frameprev /tmp/frame.ppm 3 b0181e "CINESTILL 50D   012" "EOS 6D"
```

`scale` is 1–4, the hex argument is the band colour. The output is a PPM.

## Not done yet

- **Exposure data is not in the camera line.** Shutter, aperture and ISO are
  Canon properties whose encodings this module does not decode yet; the line
  carries the camera model only. The `line2` string is built in one place
  (`fs_frame_apply`), so adding them is a local change there.
- **The frame is not composited into the in-shutter YUV path**, so a shot taken
  with the in-shutter grade gets the frame only if it is later developed.
- **Not exercised on the body.** The firmware builds and the host path compiles
  and links, but no camera was connected in this workspace to confirm the bands
  land where the host preview puts them.
