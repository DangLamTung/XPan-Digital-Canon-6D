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

```
┌──────────────────────────────────────────────┐
│ CINESTILL 50D   012                          │   band, stock brand colour
├──────────────────────────────────────────────┤   one-pixel keyline
│                                              │
│                the photograph                │
│                                              │
├──────────────────────────────────────────────┤
│ EOS 6D                                       │   band + keyline
└──────────────────────────────────────────────┘
```

- **Top band**: stock name (from the recipe's file name, uppercased) and a
  three-digit frame counter that wraps at 999.
- **Bottom band**: the camera model, from ML's `camera_model`.
- **Colour**: the band takes the brand colour of the stock family —
  CineStill red `#B0181E`, Kodak yellow `#E0A000`, Fuji green `#0A6B3A`,
  Ilford black `#141414`, neutral `#1A1A1A` for anything unrecognised.
  The text colour is chosen from the band's Rec. 601 luma, so it stays readable.
- **Type**: a 5×7 pixel font, column-major, scaled ×3 on a full-size develop
  (21 px cap height in a 48 px band).

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
