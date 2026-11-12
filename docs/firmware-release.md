# Shipping the calibrated stocks to the camera

This is how the recipes and the engine change in this repository reach a real
Canon 6D running Magic Lantern.

## What actually ships

| Piece | Where it lives on the card | Built from |
|---|---|---|
| `film_sim.mo` | `ML/modules/film_sim.mo` | `magiclantern/modules/film_sim/` (includes the SE engine) |
| Film recipes | `ML/FILM/<NAME>.fs` | `magiclantern/modules/film_sim/films/*.fs` |
| Firmware loader | `autoexec.bin` | `magiclantern/platform/6D.116/` |
| Look LUTs (optional) | `ML/FILMSIM/LOOK.BIN` or per-stock `<lut>` | `se/hosttest/fs2cube` + `tools/cube2bin.py` |

The camera resolves a stock by alias and loads its recipe from `ML/FILM/<alias>.fs`
(falling back to `ML/FILMS/`). If the card carries an older recipe than the repo,
colour drifts from what was tested on the host — which is why `install-sd.sh`
copies the whole `films/` directory and warns about stale files it finds.

## Build

```bash
./ci/release-build.sh          # recommended: tests, build, package into dist/
# or, for a quick module-only iteration:
./build.sh 6D.116 clean
```

`ci/release-build.sh` is the release gate and does, in order:

1. the SE host tests (`se/hosttest/run_tests.sh`) — refuses to package unless
   they print `ALL HOST TESTS PASSED`;
2. the ROM-tool unit tests;
3. a clean platform build (a non-clean build does not notice module source
   changes on its own);
4. a check that every module import resolves against the body;
5. a check that the zip carries the module that was just built, byte for byte;
6. packaging into `dist/`.

Artefacts in `dist/`: `magiclantern-6D.116.zip`, `autoexec-6D.116.bin`,
`film_sim-6D.116.mo`, `hosttests.log`, `RELEASE-NOTES.md`, `SHA256SUMS`.

`dist/` is generated and git-ignored on purpose: the tag and the build script
are the source of truth, not a checked-in binary.

## Install

```bash
./install-sd.sh --dry-run      # report what would change
./install-sd.sh                # find the card automatically
./install-sd.sh /Volumes/EOS_DIGITAL
```

The card is never formatted. `ML/SETTINGS` and `ML/LOGS` are preserved, so
module enable flags, slot backups and logs survive a reinstall. Every copied
file is verified against the build tree by SHA-256, and the install reports:

```
  fs    ML/FILM/ (35 stock recipes)
  mods  ML/modules/ (… fresh .mo from modules/build)
```

## The engine change in this release

`se/engine/look.h` adds `SE_HUE_LUM_FULL` (32) and `se/engine/look.c` uses it
to fade the per-hue **luma** stage over a wider chroma range than saturation and
rotation, which keep `SE_HUE_FULL` (8).

Why: a white shirt at ~8 % saturation cannot carry a 10 % per-hue brightness
step, and at the old threshold it came out as visible blotches. Saturation and
rotation are invisible at that chroma, brightness is not. Measured on the
CineStill 50D corpus the change was neutral-to-positive (4.249 → 4.220 at the
time) and it removed the artefact.

This affects every stock, not just 50D, because it is in the shared engine. The
19 calibrated stocks were re-measured after the change; results are in
`docs/stock-calibration.md`.

## Verifying a build before it goes on a card

```bash
# host: engine is correct on the reference corpus
cd magiclantern/modules/film_sim/se/hosttest && ./run_tests.sh

# host: one stock against its Dehancer corpus
python3 magiclantern/modules/film_sim/tools/calib/stock_eval.py \
  --stock "Velvia 50" --fs magiclantern/modules/film_sim/films/velvia.fs \
  --corpus xpan_comparisons/stocks/Velvia-50 --out /tmp/velvia-check
```

On the camera, after installing: enable `film_sim` in the Module menu, pick a
stock from the shelf, and confirm `ML/LOGS/FILMSIM.LOG` shows the recipe it
resolved (`ML/FILM/<alias>.fs`) — a fallback single-curve develop is the usual
symptom of a recipe that did not reach the card.

## What is not verified here

The camera-side behaviour of the new luma fade has not been exercised on
hardware in this workspace: the build compiles and the host tests pass, but no
camera was connected to confirm the on-device rendering or to diff a camera
render against the host render. That is the one step this document cannot
certify — connect a 6D, install, shoot the ColorChecker, and compare against
`xpan_comparisons/charts/colorchecker_source.jpg` graded by the same recipe.
