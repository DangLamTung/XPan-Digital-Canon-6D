# film_sim — Canon EOS 6D (1.1.6) Magic Lantern film simulation

Read this before touching the module. It exists so the project's rules and state
do not have to be re-explained every session.

## The user does not trust this agent. Every task is done twice.

This is a standing instruction, not a comment on one session. It was earned:
claims were made here that a build was installed when it was not, that a
feature worked when it had never run, and that a test passed when the test had
not been run at all.

* **Pass 1** — do the work, measure it.
* **Pass 2 is a code review, not another hash.** Rebuilding and comparing `md5`
  proves the file that landed is the file that was built; it proves nothing
  about whether the change is correct. Pass 2 means: open the code that was
  just written and the code that calls it, and answer for it line by line —
  who writes this variable, who reads it, which menu can still reach it, what
  happens when it is zero, what did the change orphan. Then re-run the tests.
  A hash-only pass 2 is a failed pass 2.
* **Report both passes.** Same result twice = verified. Different results =
  say so, show both, and report the task as **not done**.
* Two passes that share one method are one pass. `md5` against the build and
  `md5` read back off the card are two. Compiling and then saying "looks
  right" is zero.
* **Evidence must be something the user can open**: an `md5` of a file on the
  card, a line in `ML/LOGS/FILMSIM.LOG` with the task name in it, a PNG in
  `film_eval/`, a git diff. "It compiles", "should work", "I believe" are not
  evidence and must not be written as though they were.
* If something could not be tested in this environment, the sentence starts
  with **"Not verified:"** and says what blocked it.

## Hard rules

1. **Never claim camera behaviour that was not observed.** On-camera facts come
   from `ML/LOGS/FILMSIM.LOG` or from a file the camera wrote. If a claim cannot
   be backed by one of those, say "not verified".
2. **Test before reporting.** Minimum: `./build.sh` passes the import gate, the
   host tests below run, and any pixel change is checked against a known file.
   The strongest check available: develop the same frame on host and on the
   camera and compare `md5` — they must be byte-identical.
3. **No silent degradation.** If a path cannot do something, it must say so on
   screen and in the log, not produce a wrong image or a vague error.
4. **Do not ship half-working code.** The mRAW decoder was removed rather than
   shipped broken; keep that standard.

## Where things are

| path | what |
|---|---|
| `magiclantern/modules/film_sim/film_sim.c` | menu, level gating, shelf + darkroom GUI, develop driver, logging |
| `magiclantern/modules/film_sim/se/se_cam.c` | camera glue: preview develop (`se_cam_develop`), raw develop (`se_cam_develop_raw`), progress and log hooks |
| `magiclantern/modules/film_sim/se/engine/` | vendored Soup Engine: CR2 parse/LJPEG, raw pipeline (`pipeline.c`, `debayer.c`), look (`look.c`), JPEG encode |
| `magiclantern/modules/film_sim/films/*.fs` | the stock recipes (curves, hue axes, split tone, halation, grain) |
| `magiclantern/modules/film_table.h` | 33 stocks used for picture styles |
| `magiclantern/modules/film_sim/se/hosttest/` | host harnesses: `se_cam_test`, `grade_jpeg`, `rawdev`, `mrawtest` |
| `tools/florin_filmroll_1.1/` | reference implementation (FilmRoll 1.1) for comparison |
| `film_eval/` | rendered evidence, comparison sheets, backups |

## The three levels (menu entry `Level`)

| level | meaning | state |
|---|---|---|
| 1 style only | look is written into the picture-style slots; the camera bakes it into the JPEG it shoots. Develop refuses to grade twice | working |
| 2 style + grade | style at capture, then the engine adds curves, 6 hue axes, split tone, halation, grain on the embedded preview JPEG | working, byte-identical to camera output |
| 3 raw develop | decode the sensor raw, black+WB, demosaic, camera→sRGB, then the look | **in progress** — full RAW only; mRAW not implemented |

**Level 3 stays marked in progress** until the camera produces a raw-developed
JPEG this project has verified. Do not re-label it working.

### Why mRAW is the blocker

The test camera writes mRAW: `Full size 2736x1824, Raw colors: 3`, an `SOF3`
whose first component samples `2x1`. Per dcraw that means
`sraw = (h*v-1)&3 = 1`, `clrs = Nf + sraw = 4`, `jwide = (wide>>1)*clrs`, four
samples per two pixels (`luma0, luma1, Cb, Cr`), the frame stored **band-major**
(`cr2_slice = (3, 1368, 1368)` → 4 bands), predictor chain `c=0` from the previous
sample in the stream (the previous Cr), `c=1` from `c=0`, `c=2/3` from their own
previous column, and Canon's matrix `50/22929, -5640/-11751, 29040/-101 >> 14`
with `hue = 8` and camera multipliers `2040/1024/1680`. Ground truth available:
`dcraw` built at `/tmp/dcraw`, raw dump at `/tmp/dc_raw.pnm`.

## Menu contract

- `SET` advances a value, `Q` goes back. Every list entry behaves the same way.
- Actions must return immediately: no file I/O, no heavy `task_create`, no
  `msleep` inside a menu handler. Spawn a task instead.
- Submenus use `.select = menu_open_submenu` + `.children`.
- `menu_add("Film", film_sim_menu, COUNT(film_sim_menu) - 1)` — the `-1` skips
  `MENU_EOL`, whose `.name` is NULL and which otherwise asserts in ML.
- Selecting a level does **not** develop anything. Develop is always explicit:
  `Develop ▸ Last Photo`, `Develop ▸ Darkroom`, or `Develop ▸ Comparison x4`.
- `Photo` chooses which frame Develop uses (SET next, Q previous); `Develop ▸
  Comparison x4` uses `ML/SETTINGS/DEVPHOTO.TXT` when present, else the newest.

## Verification loop

```sh
./build.sh                                  # fails if a symbol is not in 6D_116.sym
cd magiclantern/modules/film_sim/se/hosttest && make
./se_cam_test  <cr2> <stock.fs> <out.jpg>    # level-2 path on this machine
./rawdev       <cr2> <stock.fs> <out.jpg> 2  # level-3 (full RAW)
./mrawtest     <cr2> <out.ppm>               # mRAW decode probe
cd - && ./install-sd.sh                      # copies, then verifies labels + md5 on card
```

Known-good regression value: `IMG_3125.CR2` + `ML/FILM/portra.fs` at level 2 must
hash to `1228a1a10ef895ee5c7090f846ba9f2f` — the camera itself produced that file.
Any engine change must still reproduce it.

Camera test with no GUI involvement:

```sh
: > /Volumes/EOS_DIGITAL/ML/SETTINGS/AUTODEV.TST      # run the 4-stock batch at boot
printf 'IMG_3219.CR2' > .../ML/SETTINGS/DEVPHOTO.TXT  # which frame
```
Output: `DCIM/100CANON/FLC1..FLC4.JPG` and `ML/LOGS/FILMSIM.LOG` (per-stage
timings plus `develop: profile look/encode/rows`).

## Measured facts, do not re-derive

- Per photo on camera: decode ~38 s, encode ~9.6 s, the entire look 0.2 s. The
  codec dominates; the look is free.
- `%u` is not implemented in ML's log formatter — it prints `u`. Use `%d` + casts.
- A 16 KB task stack overflowed at the decode step (8 KB header buffer + 8.7 KB
  `jd_state`). Both are static now; develop tasks get 32 KB.
- Reference module task stacks: shelf 16 KB, develop worker 8 KB.
- The reference module applies picture style + WB + ISO at capture; it has no
  working cube LUT (`ML/DATA/FILMROLL.CUB` is absent) and its raw path fails.

## Open items

1. mRAW decoder — Level 3 on this camera.
2. GUI is a text layout plus a scrolling film-strip band; a richer animated
   shelf is still wanted.
3. 20 of 33 stocks have no `.fs` recipe and fall back to a generated one.

## Menu: shelf first, everything else under Film Settings

* Root: **Film Shelf** (opens the shelf GUI -- picking a stock used to be three
  presses deep inside Film Look), **Film Settings**, **Develop**, **Photo**.
* `Film Settings` holds Stock, Level, Strength, Grade Quality, Saturation, Grain,
  Grain Size, Pad to 3:2, Aspect, User Look, Custom Look, Recipe, LiveView,
  Capture, Debug.
* `Mark` and `Develop ▸ Marked Set` were removed: the root entry was one photo at
  a time with no visible state, and the set was almost always empty. The
  `fs_marks[][]` array stays because `ML/SETTINGS/DEVSET.TXT` still fills it.
* `Photo` (root menu) picks the frame Develop uses.

## Develop targets: one photo, a set from the card, or automatic

* `Develop ▸ Current Photo` / `Comparison x4` / `Auto Develop` / `Darkroom`.
* `Auto Develop` (`film.sim.auto`, default 0) runs `fs_auto_task`: a watcher that
  develops a new frame once its size is stable across two 2.5 s polls, and develops
  the selected photo when the level is switched to 2 or 3. It never runs while a
  view or another develop is open.
* Unattended levers, same shape as DEVPHOTO/AUTODEV: `ML/SETTINGS/DEVSET.TXT`
  (one 8.3 name per line) + `ML/SETTINGS/AUTOSET.TST`. They are read in
  `fs_lever_task` five seconds after boot, **not** in `film_sim_init`: at init the
  card filesystem is not mounted and every FIO call fails silently.

## QEMU is not a usable test bed in this workspace right now

`qemu/run-6d.sh` fails for two separate reasons, both environmental:

1. Three qemu processes from earlier sessions are wedged in state `U`/`UE`
   (PIDs change, but `ps -o stat` shows `UE`) and hold read locks on `qemu/sd.img`
   and `qemu/cf.img`. A new guest cannot take the write lock, so it exits with
   `Initialization of device ide-hd failed: Failed to get "write lock"`.
2. Even on a private copy of the images the emulated firmware stops at ~365 ms of
   guest time (`[EOS] MEMTX invalid read ... 0x3f800000`) and the module init at
   890 ms is never reached; the qemu process then sits at ~3% CPU instead of ~100%.
   A develop that takes 32 s on the camera cannot finish there.

So: verify on the camera, or on the host (`se/hosttest`), and do not report a QEMU
run as evidence unless the card shows the files afterwards.

## Screens live in gui/, not in film_sim.c

* `gui/fsg.c` draws both screens through exactly two primitives (`fsg_fill`,
  `fsg_text`). `gui/fsg_ml.c` implements them with `bmp_*` on the camera;
  `gui/fsg_host.c` implements them into an RGB buffer so the same
  code can be rendered on a Mac. `gui/fsg_dat.c` is shared too: it parses the
  canister artwork through `fsg_file_*()`, which each backend supplies (FIO vs
  stdio), so the preview reads the very same `ML/data/FILMCAN.DAT`.
* **The camera palette is 8-bit indices, not RGB.** Only the constants in
  `gui/fsg.h` may be drawn; each one has an exact `ml_color()` mapping. No
  gradients, no blends, no per-stock colours -- those cannot exist on the panel.
* The preview tooling lives in the module, not in the parent repo: `gui/fsg_host.c`
  is the backend `se/hosttest` builds `creatorwrite` from,
  `tools/gui_preview/preview.c` drives the screens and `render.py` composites
  them. A standalone clone of this repo has no parent, so anything the tests
  need must be in here.
* Preview, from the module repo root:
  `gcc -O1 -Imodules/film_sim modules/film_sim/tools/gui_preview/preview.c
  modules/film_sim/gui/fsg_host.c modules/film_sim/gui/fsg.c
  modules/film_sim/gui/fsg_dat.c -o /tmp/fsg_preview &&
  /tmp/fsg_preview <path to FILMCAN.DAT> &&
  python3 modules/film_sim/tools/gui_preview/render.py`
  writes `/tmp/fsg_*.png` and `/tmp/fsg_sheet.png`. Run the preview with no
  argument to see the no-artwork fallback; rendered frames are kept in
  `film_eval/gui/`. `render.py` renders whatever frames the preview wrote --
  it used to hold a hardcoded list that rotted.
* Animation is state in the caller: `fs_anim_scroll` eases toward the selected
  stock (1/256 frame units) and the shelf loop redraws on a 70 ms clock, so the
  strip scrolls instead of jumping.

## LiveView display filter: the buffer flip belongs in ML

`src/tweaks.c` `display_filter_step()` now points
`YUV422_LV_BUFFER_DISPLAY_ADDR` at `display_filter_buffer` after every processed
frame, for `CONFIG_CAN_REDIRECT_DISPLAY_BUFFER_EASILY` bodies. Before that it was
done only from `display_filter_lv_vsync()`, i.e. on GUI state changes, while
Canon re-points the address at its own buffer for each new frame -- the panel
alternated between graded and ungraded frames (the flicker the 6D showed, and
that QEMU never did because it had the inline flip already).

A module must not write that register itself. That was tried and reverted: it
also sidesteps the frame-completeness logic ML does around it.

## Pre-flight before saying any screen works

These four checks each caught a bug that had already been reported as fixed.
Run them before writing the word "done".

1. **Keys**: read `platform/6D.116/gui.h` for the real `BGMT_*` codes and drive
   screens from `CBR_KEYPRESS_RAW`. `CBR_KEYPRESS` delivers *portable* codes and
   on this body the d-pad and SET translate to `0` — the log was full of
   `key 0` while the buttons were being pressed.
2. **Menu names**: `grep -rn 'menu_add(' modules/*/*.c`. Two modules adding the
   same top-level name is the "menu overlaps Magic Lantern" symptom; `film_lv`
   and `film_sim` both used `"Film"` until it was caught.
3. **Read `ML/LOGS/FILMSIM.LOG` before proposing a cause.** Every wrong guess in
   this project so far was answered by a line already in that file.
4. **Nobody else's key codes are our key codes.** Florin's module is a reference
   for structure, not for constants; his build and this one do not share a
   platform header version by assumption.
