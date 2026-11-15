# The 6D firmware: autofocus and Wi-Fi, what is established

Sources: `qemu/6D/ROM0.BIN` (8 MB at `0xF0000000`), `qemu/6D/ROM1.BIN` (16 MB at
`0xF8000000`), the ML symbol table `platform/6D.116/build/6D_116.sym`,
`magiclantern/src/property.h`, and `film_eval/rom/RAM4.BIN` (256 MB).
Tools: `tools/rom/romscan.py`, `tools/rom/propscan.py`.
This document replaces the round-by-round log that preceded it; the log is in
git history, the facts are here.

## Identity of the images

| image | size | load address | note |
|---|---|---|---|
| ROM0 | 8 MB | `0xF0000000` | **no code**: 0 `push {..,lr}` prologues in 8 MB |
| ROM1 | 16 MB | `0xF8000000`, **also `0xFF000000`** | 33,381 prologues, 268,575 `bl` |
| RAM4 | 256 MB | — | ML's memory, and **contains the AF/AE code block** |

The second mapping is the important one: every handler and data pointer in the
firmware uses `0xFF...`, and `file offset = address - 0xFF000000`. Disassembling
at `0xF8000000 + (addr - 0xFF000000)` gives the same bytes.

ROM1 also references memory above its own end: **2,681** literals point into
`0xF9000000`-`0xF9FFFFFF` and **1,989** into `0xFA000000`-`0xFAFFFFFF`, and the
socket code calls `0xF90554F8` / `0xF905555C`. ML's `consts.h` declares
`ROM1_SIZE 0x01000000`, so the dump stops short of that region.

## Why string search does not find code

Every trace message, task name and debug-parameter name is reached by
**index**, not by pointer. All three searches come back empty, repeatedly:

- absolute 32-bit literal pointing at the string: 0 hits
- the `0xFF` alias of the same string: 0 hits
- PC-relative `add rX, pc, #imm` landing on it: 0 hits

This is the single reason the static work stalls, and it is the same for AF and
for Wi-Fi. The fix is a live trace from a module, not another search.

## Autofocus: the control surface (established)

The property dispatch table is at `0xF83149DC`, entries of the form
(property id, handler):

| property ids | handler |
|---|---|
| `0x80050000 0x80050002 0x80050005 0x8005000A` (`PROP_LV_LENS`, `PROP_LV_FOCUS_DONE`, `PROP_LV_LENS_STABILIZE`, `PROP_HALF_SHUTTER`) | `0xFF3131F4` |
| `0x8005000B 0x8005000C 0x8005000D` | `0xFF313128` |
| `0x80050008` | `0xFF313164` |
| `0x80050010 0x8005001B 0x8005001E 0x80050027` | `0xFF3120A0` |
| `0x80050029` = `PROP_LV_AF_RESULT` | `0xFF3131A0` |
| `0x80050028` | `0xFF3131DC` |
| `0x8005002B` | `0xFF3132FC` |
| `0x80050033` | `0xFF313340` |

A generated version of this table is `docs/af-property-map.txt`
(`tools/rom/afmap.py`, which reads the dispatch table, the descriptors and ML's
`property.h` in one pass). **It is a partial view**: it covers the dispatch table
at `0xF83149DC` only. `PROP_LV_LENS_DRIVE_REMOTE` (`0x80050013`) is used by ML's
own `lens_focus()` (`src/lens.c:808`) to move the lens, and it appears in three
property-id lists in the ROM (`0xF8B69860`, `0xF8B698F8`, `0xF8D48A38`) but has
neither a descriptor in the tables found so far nor an entry in this dispatch
table. So the AF family is larger than what is mapped, and the unqualified
phrasing "the AF property family" should not be used for it.

The dispatch table covers **30 descriptors** in the table at `0xF8B6EB64`
(16-byte entries: id, flags, a third field, index). The third field is **not**
the payload length: `PROP_AFPOINT` has 1 there while ML's `property.h` records
`len=70`, and the handlers copy 3 and 11 bytes for properties whose field is 9.
What the field means is still open; it is printed as `field3` in
`docs/af-property-map.txt` rather than being called a size.
Full list: `docs/property-descriptors.txt`.

The handlers are thin. `0xFF3131F4` copies two bytes of payload into a 3-byte
stack block; `0xFF3120A0` copies one byte into a 2-byte block; both then call the
low-RAM dispatcher `0x5FB8`. `0xFF3131DC` instead tail-calls `0xFF1302E8` with a
**24-byte command block**.

`0xFF1302E8` is the AF command function: prologue `push {r3-r9, sl, fp, lr}`,
lock via `0xA5CC`, copy via `0x6BB98`, eight calls to `0xFF130828` (a wrapper
around `0x39F54` that logs on error), plus `0x90F4`, `0x91AC`, `0x904C`.

**ML already drives exactly this surface**: `set_af_point()`
(`src/af_patterns.c`) writes `PROP_AFPOINT`, `lens_focus()` (`src/lens.c`)
sends a step command (`-3..3`, `0x8000 - step` for negatives), `set_afma()`
(`src/afma.c`) writes the micro-adjust values. Those land on the handlers above.

## Autofocus: the algorithm's architecture (established)

Two AF systems, both named in the ROM's trace table.

**Through the viewfinder: phase difference.** The ROM carries the calibration
model:

```
PhaseDiffCarrierWaveCenterL2R / R2L        : %d(angle)
PhaseDiffCarrierWaveSideL2R / R2L          : %d(angle)
AdjustPhaseDiffCarrierWaveCenter/Side L2R / R2L
AdjustPhaseDiffCarrierWaveSideL2RLoTemp / R2LLoTemp
AdjustPhaseDiffStationaryWaveReverse / Same
dl_SetDutyAForCarrierWaveCenter/Side L2R / R2L
dl_SetDutyAForStationaryWaveReverse / Same
```

So: the AF sensor has **two line pairs** (center, side), correlation runs in
**both directions** (L2R, R2L), a **duty** calibration applies to the sensor's
carrier (stripe) pattern, there is a **low-temperature** variant, and a
**stationary-wave** case is calibrated for same and reversed phase. The runtime
value is an **angle**, which becomes a defocus, which becomes a lens drive.

**LiveView ("TvAf"): contrast, evaluated per frame by AEWB.** The ROM has
**165 `AEWB_*` names**; the ones that matter:

```
AEWB_CTRL_ExecMainProcess                  the per-frame evaluation loop
AEWB_CTRL_RequestEmdDriveWithoutBadFrame    the drive decision
AEWB_CTRL_SetCntTVAFBadFrame                bad-frame counter
AEWB_AFAE_EMDLOCK_ONETIME / _OFF            lens-drive lock around a measurement
AEWB_AEL_SET_AFAE / RESET_AFAE
AEWB_DSTOCK_GetAFAEInfo / GetAFBvForAFAE / GetBadFrameStatus
```

**The controller is an event/state machine.** Actions named in the trace table:

```
AfCtrl_Act_Ready / Suspend / Ignore(%d)
AfCtrl_Act_SetLensParameter / SetLensParameterRemote
AfCtrl_Act_StartLensDriveRemote / EndLensDriveRemote
AfCtrl_Act_EmdDriveResult( Comp:%d ) / CompleteEmdDrive
AfCtrl_Act_CompleteAe_ForTvAf / CompleteAfResult
AfCtrl_Act_ContinuousAfStart / ContinuousAfStop
AfCtrl_Act_TvAfStart / TvAfStop
AfCtrl_ExecuteEvent : Undefined( Event:%d, State:%d )
AfCtrl_PropertyCBR PROP_LV_AF(%#lx)
```

Reading: the AF controller only **queues** work; AEWB evaluates each frame;
`EmdDriveResult` is where a drive ends and the next decision is made;
`RequestEmdDriveWithoutBadFrame` is that decision.

Where the AF code lives: **RAM `0x03C000`-`0x043000`, 83 functions**, next to the
names `AfWrDmacCompleteCBR`, `AfWrDmacAbortCBR`, `AfCompleteCBR`,
`AfOverRunErrorCBR` (`docs/af-function-map.txt`). That block is mostly property
reads (82 calls to the getter `0xFF1319C4` out of 289 calls into the ROM), so the
metric is not there.

**Not established:** the metric formula itself.

## Wi-Fi: the stack is present and this body has the radio

The 6D was the first EOS with built-in Wi-Fi and GPS, and the firmware carries
the whole stack: `WlanMgr`, `WlanSDIODriver`, `WlanSdcom`, NWCommu, the LAN
dialogs, DLNA, WFT HTTP, the Marvell **88W8787** SDIO driver banner
(`w8787-Ax, RF878X, FP197, 14.197.9.p255`), WPA (`wpaTask`), DHCP (`dhcpsTask`),
and Eye-Fi support.

Where it lives:

| region | contents | inside the 16 MB dump? |
|---|---|---|
| `0xFF26xxxx` | NWCOM manager: Init, FTP, discovery, connect, reconnect, GUI events | yes |
| `0xFFD2xxxx` | SDIO transport (`SDIOAsyncCBR`, async completion) | yes |
| `0xF905xxxx` | socket/TCP layer | **no** |

Known entry points (from `docs/wifi-functions.md`): `NWCOM Init 0xFF261714`,
`TransmissionChangeCBR 0xFF2616E8`, `NetworkStatusCBR 0xFF261B00`,
`Wizard 0xFF261F70`, `Order 0xFF262358`, `FTP 0xFF261974`,
`ChangeLogicalConnect 0xFF2634B8`, `ReConnect 0xFF2638C4`, `GUIEvent 0xFF266334`.

**`NWCOM Init` cannot be called with `r0 = 0`.** It dereferences its argument:

```
ff261714  push {r2, r3, r4, r5, r6, lr}
ff261718  mov  r4, r0
ff26175c  ldr  r3, [r4, #32]
ff261768  ldr  r3, [r4, #28]
ff261778  ldr  r3, [r4, #40]
```

and it is registered as the **event-1 handler of a component table** at
`0xF8B69458`, so it is a framework callback with an owned context, not an entry a
module can jump into. The gated `mlwifi` rung-2 call would data-abort.

The missing piece is the **on/off switch**: the name `PROP_WIFI_SETTING` exists
in the firmware's name table at `0xFF19637C`, but its id is not tied to it
anywhere in the image, and modules cannot read properties (`prop_get_value` is
commented out in `src/property.h`) -- only `PROP_INT`, which fires on change.

## Property descriptor tables

`tools/rom/propscan.py` finds 7 runs of descriptors (16-byte entries:
id, flags, size, index) in ROM1; `docs/property-descriptors.txt` lists all of
them. The `0x8004xxxx` family has 67 descriptors. That is how the candidate
`0x80040055` was **rejected**: the ids run `0x80040053` -> `0x80040056` with
nothing between, so it is not a registered property.

## What stands between here and the answer

| goal | what is missing | how it is obtained |
|---|---|---|
| AF metric | the identity of the task that publishes the AF result | `aftrace` module: logs value + `get_current_task_name()` + ms for `PROP_LV_AF_RESULT`, AF point, mode, BV |
| Wi-Fi on | the id of `PROP_WIFI_SETTING` | `mlwifi` module: Watch Props (116 ids across `0x8003`/`0x8004`) while toggling Wi-Fi in Canon's menu; List Tasks to see `wpaTask`/`dhcpsTask` |
| Wi-Fi code past `0xF905xxxx` | a dump wider than 16 MB | set `ROM1_SIZE` to `0x02000000` and re-dump |

Both modules are built into `magiclantern.zip`, both are read-only in every
path they run today, and both were checked to import only symbols that
`6D_116.sym` exports.

## The lesson that cost the most rounds

`docs/wifi-functions.md` (2026-11-06) and `docs/wifi-recipes-plan.md` already
recorded the `0xFF` alias, the index-based string tables, the NWCOM addresses and
the correction that this body has Wi-Fi. Several rounds were spent re-deriving
them. Read the existing documents first.

## Confidence: what to trust in this document, and how it was established

The claims here are not all of the same kind, and three of them were wrong at some
point in this work. This table records the basis for each so a later reader can
re-check rather than trust.

| claim | basis | confidence |
|---|---|---|
| ROM1 is code, ROM0 is not | counted `push {..,lr}` prologues: 33,381 vs 0 | measured |
| ROM1 is also mapped at `0xFF000000` | disassembled `0xFF3131F4` and got a valid prologue; 4,670 literals point into `0xF9000000`+ | measured |
| The dispatch table at `0xF83149DC` maps 8 id groups to handlers | read as 16-byte records and cross-checked against the handlers' prologues | read from image |
| `PROP_LV_AF_RESULT` → `0xFF3131A0` | table entry, and the handler's shape (copy 7 bytes, mask two payload bytes, call `0x5FB8`) | read from image |
| The AF map is **partial** | `PROP_LV_LENS_DRIVE_REMOTE` (`0x80050013`) is used by ML's `lens_focus()` yet absent from the map | proven by contradiction |
| The descriptor's third field is not a payload size | `PROP_AFPOINT` has 1 there, ML records `len=70`; handlers copy 3 and 11 bytes for properties whose field is 9 | proven by contradiction |
| Phase-difference AF with center/side lines, L2R/R2L, duty, LoTemp | ROM strings `PhaseDiffCarrierWave*`, `dl_SetDutyAFor*` | string evidence |
| AEWB evaluates per frame and decides with `RequestEmdDriveWithoutBadFrame` | ROM strings `AEWB_CTRL_*` | string evidence |
| The AF code block sits at RAM `0x03C000`-`0x043000` | 83 prologues plus the names `AfWrDmacCompleteCBR` and friends | read from image |
| `NWCOM Init` needs a context in `r0` | `ldr r3, [r4, #32]` in its first instructions | read from image |
| `NWCOM Init` is a component event handler | the single literal reference lands in the table at `0xF8B69458` | read from image |
| Wi-Fi hardware is present on this body | the firmware ships `WlanMgr`, `NWCommu`, `wpaTask`, `dhcpsTask`, the 88W8787 banner; Canon's spec for the 6D lists Wi-Fi and GPS | string + external |
| ML can reach the network today without firmware calls | Develop writes `DCIM/<dir>/IMG_nnnn.JPG` and replaces the camera JPEG, so Canon's app lists it | read from code |
| The metric formula | not established | unknown |

Two rules this table enforces: a claim derived from a string is weaker than one
read out of a table, and a claim contradicted by code elsewhere in this repository
loses regardless of how often it was repeated.

## Where the firmware actually touches pixels (measured)

Counting `ldrb` instructions per 256 KB of ROM1 gives a rough map of which code
reads bytes in bulk, and therefore where image work happens at all. Top windows
(full table in `docs/rom-code-density.txt`):

| window | ldrb |
|---|---|
| `0xFFFC0000` | 20,048 |
| `0xFFA40000` | 10,237 |
| `0xFFA80000` | 8,815 |
| `0xFF580000` | 7,999 |
| `0xFF3C0000` | 7,772 |
| `0xFFCC0000` | 7,041 |

The AF unit (`0xFF130000`, 326 `ldrb` over 64 KB) and AEWB
(`0xFF2A0000`/`0xFF2B0000`, 554 each) are **an order of magnitude below** these.
The dense regions hold tables that look like codec tables (Huffman-shaped), so the
heavy byte work in this firmware is JPEG and image transport, not metering.

What this establishes: there is **no full-frame pixel loop in the AF or AEWB
code**. What it does *not* establish is where the metering happens -- the AEWB
code does touch peripherals (`0xC0080000`, `0xC0080500`, `0xC0080700`,
`0xC0E00000`, `0xC0E2008C`), which is consistent with a hardware evaluation block
reading the LiveView stream and the firmware reading results, but I have not
identified those blocks and cannot claim it.

## Round 35: the firmware can log the AF internals itself

The strings around `0xF824DDB8` describe an **AF logging facility inside the
firmware**:

```
B:/%02d%02d%02d%02d.AFL          <- the file it writes, dated MMDDHHMM
[AFLOG] [%s] FOpen Success
[AFLOG] [%s] FOpen Error
[AFLOG]FWrite Size (%d)
[AFLOG]FWrite Size Error (%d=>%d)
SPECIAL_OPTION_AFAE DEBUG MODE ON
SPECIAL_OPTION ULTRA DEBUG MODE ON
```

and a property that gates the diagnostic modes. Note where that property is read:
the strings next to `PROP_SPECIAL_OPTION = %d` are `fmChangeProperty : Unknown ID`,
`fmSuspend`, `fmResume`, `fmSetInitialCardInfo`, so the reader is the **file
manager**, not the AF subsystem -- the flag is global, not lens state:

```
PROP_SPECIAL_OPTION = %d
fcsChangeAckCBR : PROP_SPECIAL_OPTION (%d)
SPECIAL_OPTION AF MOVIE GERO ON / OFF
SPECIAL_OPTION_LVAE_GERO_MODE:0x%08X
```

If the AFAE debug mode can be switched on, Canon's own code writes the AF
internals to `.AFL` on the card -- the metric values, the states, the drives --
and the algorithm stops being a disassembly problem.

What is *not* known, and what I did not guess:

* the id of `PROP_SPECIAL_OPTION`. The only ids near its log strings are
  `0x8003000c` (which ML knows as `PROP_CARD_RECORD_B`) and `0x80040001`;
  proximity is not evidence, and this session has already rejected one candidate
  for exactly that reason (`0x80040055`);
* the bit or value that selects AFAE debug over the other special options;
* whether the 6D has a `B:` device at all -- the log path is `B:/`, and this body
  has one card slot, so the writer may fail even with the mode on.

The safe next step is not to write the property blind: it is to watch it. If
`mlwifi`'s watcher is extended to `0x80030000`-`0x8003003F` (it already covers
that block) and a service-mode or PTP tool ever toggles the option, the id and
value appear in the log. Without such a tool, the id has to come from
disassembling the component that logs `fcsChangeAckCBR`.

## Round 38: the evidence points at hardware metering, which changes what "the algorithm" can be

Four measurements now agree that the firmware does **not** compute a focus metric
from pixels:

1. **Pixel-read density.** AF (`0xFF130000`, 326 `ldrb`/64 KB) and AEWB
   (`0xFF2A0000`, 554) are an order of magnitude below the codec regions
   (`0xFFFC0000`: 20,048 per 256 KB). A full-frame metric over a 5472x3648 buffer
   would need far more byte reads than either region contains.
2. **No 64-bit accumulation.** Across the whole AF unit: 56 `mul`, **0**
   `umull`/`smlal`/`umlal`/`smull`. A correlation over paired lines accumulates
   products wider than 32 bits; none of that is here. AEWB has 6 `umull`, also
   far too few.
3. **The AF code block in RAM has no 64-bit maths either** -- 20 `mul`, no
   `umull`/`smlal` (`docs/af-function-map.txt`).
4. **AEWB reads a 7-bit value out of a peripheral block.** This one is an
   instruction sequence, not a literal count:

   ```
   ff2a3dcc  ldr   r0, [pc, #376]   @ 0xff2a3f4c -> 0xC0080500
   ff2a3dd0  ldrh  r0, [r0, #8]     @ read a 16-bit register at 0xC0080508
   ff2a3dd8  and   r5, r0, #127     @ keep 7 bits
   ff2a3ddc  ldr   r0, [r4, #64]
   ff2a3de4  cmp   r0, r5
   ff2a3de8  movcc r5, r0           @ clamp against a computed value
   ```

   Confirmed by pc-relative-load analysis: of the five peripheral addresses in
   the AEWB region, `0xC0080000` is loaded at `0xFF2A1E58` and `0xFF2A7360`,
   `0xC0080500` at `0xFF2A3DCC`, `0xFF2A594C` and `0xFF2A1E28`-adjacent slots,
   `0xC0080700` at `0xFF2A2E14`, and `0xC0E2008C` at `0xFF2B0EA4`. Several other
   occurrences of the same constants are table data, not code -- so counting
   literals would have overstated this, and the instruction is what makes it real.

The reading: the DIGIC evaluates the LiveView frame in hardware (the interrupt
name table even carries a `WB_AE` block), and the firmware's autofocus is the
**control loop around those results** -- which is exactly what the `AfCtrl_Act_*`
state machine and the AEWB decisions describe.

What that means for the goal, stated plainly: the part of Canon's AF that a ROM
image can give -- the control surface, the state machine, the drive/decision
sequence, the phase-difference **calibration model** -- is mapped. The metric
itself, if it is computed in the DIGIC's metering block, is **not in this image at
all** and cannot be recovered by disassembly.

What is still open is whether point 4 is the whole story. The blocks are
unidentified, `0xC008xxxx`/`0xC0E0xxxx` are documented nowhere in this repository,
and I have not seen a register map or a datasheet. Until one of those exists, this
stays a conclusion from four consistent measurements, not a proven fact.

## Round 40: the peripheral in point 4 is probably a data path, not a meter

Following the block whose base is `0xC0080000` further weakens the
"hardware metering" reading of round 38.

The function at `0xFF2A1E2C` initialises it like this:

```
ff2a1e40  mov  r0, #36            @ allocate 36 bytes
ff2a1e44  bl   0xD068
ff2a1e58  ldr  r0, [pc, #1068]    @ 0xFF2A228C -> 0xC0080000
ff2a1e5c  str  r1, [r0, #16]      @ 0xC0080010
ff2a1e64  str  r1, [r0, #84]      @ 0xC0080054
ff2a1e68  mov  r1, #0x7000
ff2a1e6c  add  r0, r0, #0x4000
ff2a1e70  str  r1, [r0, #128]     @ 0xC0084080
ff2a1e78  str  r1, [r0, #132]     @ 0xC0084084  (512)
ff2a1e80  add  r0, r0, #0x2000
ff2a1e84  str  r5, [r0, #268]     @ 0xC008610C
ff2a1e8c  str  r5, [r0, #428]     @ 0xC00861AC
```

Writing a count (`0x7000`), a size (`0x200`), then clearing a run of offsets
(`0x10C`, `0x18C`, `0x1AC`, `0x0C`) is the shape of a **DMA / transfer engine**
being set up, not of a metering block. Strings near the same constant mention
`ASIF` (`/ASIF/ASIF.c`) and USB endpoints, which fits an image/data path.

So the correct statement of point 4 is narrower than round 38 made it:

* true: AEWB **reads** a 16-bit register and keeps **7 bits** of it
  (`ldrh r0, [r0, #8]`; `and r5, r0, #127` at `0xFF2A3DD0`),
* true: the same address range is initialised like a transfer engine,
* **not established**: that this block computes the AE/AF metric. The earlier
  sentence "the DIGIC evaluates the frame in hardware" is now an unproven
  hypothesis, and the four measurements in round 38 support only the weaker claim
  that **the metric is not computed in the AF or AEWB firmware**.

I am leaving the round-38 text in place rather than deleting it, because the
correction matters more than the tidy document: two rounds in a row, a plausible
reading of the same evidence changed direction.

## Round 41: the absence of DSP maths in AF/AEWB is now a measured absence

Counting `SMLAxy` -- the ARMv5TE halfword multiply-accumulate, the instruction a
correlation or a DCT actually uses -- gives a **positive control** that the
earlier counts lacked:

| region | `SMLAxy` |
|---|---|
| `0xFFA40000`-`0xFFA5FFFF` (codec) | **305** |
| `0xFF130000`-`0xFF13FFFF` (AF unit) | **6** |
| `0xFF2A0000`-`0xFF2BFFFF` (AEWB) | **0** |
| `0xFF3C0000`-`0xFF3CFFFF` | 0 |

Across the whole 16 MB image there are 3,740 `SMLAxy`, and they concentrate in
`0xFFA1xxxx`-`0xFFB4xxxx` (top windows: `0xFFB10000` 208, `0xFFB40000` 186,
`0xFFAA0000` 180). The AF and AEWB regions are not in that list at all.

Why this matters: in round 38 I reported "no 64-bit accumulation", which could
have been a detector that simply does not fire on this code. The codec region
proves the detector fires -- 305 hits where heavy DSP work exists. So the near-zero
in AF/AEWB is a **measured absence**, not a measurement artifact.

That strengthens exactly one claim, and only this one: **the focus metric is not
computed by the AF or AEWB firmware**. It says nothing about where it *is*
computed, and the round-40 correction about the `0xC0080000` block still stands.

## Round 42: the region I called "AEWB" is the SDIO driver -- rounds 38-41 measured the wrong bytes

While checking the audio/ASIF finding, I read the strings inside
`0xFF2A0000`-`0xFF2C0000`, the window that rounds 38-41 called "AEWB":

```
InitializeSDIODriver : sdioDrvCreateStateObject Error
./SdioDrv/SdioDrv.c
OpenSDIODataPipe : Parameter Error
sdioDrvExecAsyncFunction : Length Error
```

That is the **SDIO driver**, not AE/AF. The name "AEWB" was attached by proximity
in an early round and never checked against the window's contents.

So these conclusions from rounds 38, 39 and 41 are **withdrawn** as stated, because
their measurements were taken on the wrong bytes:

* "AEWB has no DSP maths" -- the window was SDIO;
* "the peripheral in the AEWB code is a data path / a meter" -- the peripheral
  belongs to whatever module lives there, and the neighbouring strings are
  `/ASIF/ASIF.c` with `SetSamplingRateToASIF`, `StartASIFDMAADC`,
  `SetASIFADCModeInterleaveINT16` -- **ASIF is the audio ADC/DAC with DMA**, so
  that block is audio, not metering.

What remains true: the AF **command** function at `0xFF1302E8` is AF (it is
reached by a tail call from the `PROP_LV_AF_*` handler, verified), and the
property handlers at `0xFF31xxxx` are AF (reached from the dispatch table).

Where the AF/AE *evaluation* code actually is: the AEWB and `AfCtrl` **strings**
live at `0xFFB68xxx`, `0xFFCCDxxx`, `0xFFCFAxxx` and `0xFFD04xxx`, so the
subsystem is in the `0xFFB6xxxx`-`0xFFD1xxxx` range. A first measurement there
gives, over 1,728 KB: **33,812 `ldrb`, 1,050 `SMLAxy`, 3,281 `mul`** -- far more
DSP-shaped than the window I was looking at. That range has not been split into
modules yet, so it is a place to look, not a conclusion.

The lesson is the same one that cost round 40: a region label guessed from a
nearby string is not evidence, and every measurement inherits that guess.

## Round 43: the AE/AF modules, located by their own source paths

Instead of guessing a window, search the image for the firmware's own source-path
strings. Every AEWB/AF unit names itself:

| path string | address | what it is |
|---|---|---|
| `./AEWB/AEWB.c` | `0xFF103504` | top of the subsystem |
| `./AEWB/AEWBState.c` | `0xFF24B25C` | state machine |
| `./AEWB/ColorController/ColorController.c` | `0xFF24E618` | colour |
| `./AEWB/AECalc/AECalc.c` | `0xFF427354` | AE calculation |
| `./AEWB/AECalc/AEProc/LiveViewAEProcSubFunc.c` | `0xFF42FBA4` | LiveView AE helpers |
| `./AEWB/AECalc/AEProc/LiveViewAEProcGeroData.c` | `0xFF431758` | AE data |
| **`./AEWB/AF/AFProc/LiveViewAFPUtility.c`** | **`0xFF4324C4`** | **LiveView AF processing** |
| `./AEWB/AECalc/AEProc/LiveViewAEProcMetering.c` | `0xFF5CB0E0` | **metering** |
| `./AEWB/WBCalc/WBCalc.c` | `0xFF429250` | WB |
| `./AEWB/AEWBCommon/AEWBDataStocker.c` | `0xFFCFE7C0` | data stocker |
| `./AEWB/AEWBCommon/AEWBPropControl.c` | `0xFFD03BE4` | property control |
| `./AEWB/AEWBRegister/AEWBRegister.c` | `0xFFD1B7D0` | registration |

So the evaluation code is in **`0xFF42xxxx`-`0xFF43xxxx`** and the **metering** in
**`0xFF5Cxxxx`**, and these addresses are now tied to names by the image's own
strings rather than by my guesses.

Measured there (positive-control rules from round 41):

| window | size | `ldrb` | `ldr` | `SMLAxy` | `mul` |
|---|---|---|---|---|---|
| `0xFF5C0000`-`0xFF5E0000` (metering) | 128 KB | 3,197 | **4,788** | 3 | 92 |
| `0xFF420000`-`0xFF440000` (AECalc/AFProc) | 128 KB | 3,078 | 4,661 | 25 | 101 |

The load density here (about 37 `ldr`/KB) is far above the codec's, and the work
is word-oriented rather than byte-oriented: this is where metering and AF
processing actually live. The region holds 241 functions; the heaviest
(`0xFF5DB528`, 10,904 bytes, 332 `ldrb` + 425 `ldr`) is where the next pass starts.

Everything in rounds 38-42 that used the `0xFF2A0000` window stays withdrawn.

## Round 44: the metering loop, and the weighted sum behind it

Inside the metering window the tightest accumulation loop is at
`0xFF5C4A5C`-`0xFF5C4AB4`. It walks a list of **zone records of 16 bytes**
(`add r5, r4, sl, lsl #4`), and for each one:

```
ff5c4a70  ldrh r1, [r5, #40]     @ zone field at +0x28
ff5c4a78  sub  r0, r0, r1
ff5c4a7c  add  r0, r0, #56
ff5c4a80  strh r0, [r5, #46]     @ store at +0x2E
ff5c4a8c  add  r0, r5, #40
ff5c4a90  bl   0xFF42EEEC        @ clamp/update helper
ff5c4aac  cmp  sl, r7
ff5c4ab0  blt  0xFF5C4A5C        @ next zone
```

So metering is organised as **zones with 16-bit coordinates**, each evaluated by a
call into `AECalc` (`0xFF42xxxx`), not as one pass over the frame.

The helper that does the arithmetic sits at `0xFF42EF18`:

```
ff42ef1c  ldrh r3, [r0]
ff42ef20  ldr  ip, [r2, #4]
ff42ef28  mul  r3, ip, r3           @ weight * zone value
ff42ef2c  add  r3, r3, r3, lsl #1   @ *3
ff42ef30  lsr  ip, r3, #10          @ /1024
ff42ef34  ldrh r3, [r0, #2]
ff42ef38  mul  r3, r4, r3           @ next weight * next value
ff42ef44  add  ip, ip, r3, lsr #10  @ accumulate
```

That is a **weighted sum with a 1/1024 scale** over the 16-bit fields of a zone
record -- the AE metering core, at `0xFF42EF18`, reached from the zone loop at
`0xFF5C4A5C`.

`0xFF42EEEC` is a saturation clamp for the same records (`ldrsh ip, [r0, #6]`,
compare against two limits, `strh` the result, write a flag at `+0x0E`).

Next: the same shape should exist for AF over the AF frame -- a weighted or
difference-based sum -- and that is what the next pass looks for in
`0xFF4324C4` (`LiveViewAFPUtility.c`) and its neighbours.

## Round 45: the AF utility window, mapped but no metric in it

`LiveViewAFPUtility.c` is the only AF source path in the image, at `0xFF4324C4`,
and its window `0xFF430000`-`0xFF43FFFF` holds **183 functions**. Ranked by memory
traffic, the heaviest are

```
0xFF43DD04  1708 bytes  47 ldrb  84 ldr
0xFF433FD4  1376        33       39
0xFF43D6AC  1624        22       71
```

and none of them reads a frame: the biggest (`0xFF43DD04`) is AF setup and
logging (calls `0xFF437EB0`, `0xFF437EC8`, `0xFF437EE0`, then formats a message);
`0xFF436E6C` allocates 264 bytes and copies a record; `0xFF436864` switches on an
AF mode (compares 2, 6, 7) and calls that record builder.

The difference-heavy loop in the window (`0xFF436864`-`0xFF436AB4`, 23 `sub`/`cmp`
in 149 instructions) is a mode dispatch, not a gradient sum.

So the picture is consistent with round 44 from the other side: **metering has
the arithmetic** (`0xFF42EF18`, weighted sum with a 1/1024 scale) and the AF
utility does not have a pixel metric of its own. The hypothesis this suggests --
that LiveView AF consumes the metering zone values rather than computing its own
-- is testable and not tested: it needs the identity of the zone-array global,
which the metering loop reaches through `r4` and which I have not resolved.

Stating the round honestly: one window mapped, one hypothesis formed, no new
metric found.

## Round 46: AF code calls into the metering module, 30 times

The hypothesis from round 45 -- that LiveView AF consumes metering values instead
of computing its own -- gets its first structural support from call directions:

| direction | calls |
|---|---|
| AF window (`0xFF43xxxx`) -> metering (`0xFF5Cxxxx`) | **30** |
| AF window -> `AECalc` (`0xFF42xxxx`) | 0 |
| metering -> AF window | 73 |
| metering -> `AECalc` | 157 |

The AF window's metering calls concentrate on one function:

```
16 x 0xFF5CD3F8     3 x 0xFF5CDA98     then 0xFF5CF488/4B0/4D8/508/D14/EB4, once each
```

`0xFF5CD3F8` is a **jump-table dispatcher**:

```
ff5cd3f8  push {r4-r8, lr}
ff5cd404  ldr  r0, [r0, #12]      @ a type/id
ff5cd424  cmp  r0, #12
ff5cd430  addcc pc, pc, r0, lsl #2   @ 13-entry table
ff5cd43c  b    ... (case 1)
ff5cd440  b    ... (case 2)
...
```

with cases that move 16-bit fields between the caller's record and a state
structure (`ldrh r0, [r5, #10]; strh r0, [r4, #6]`) and one that calls
`0xFF7661F0` for the actual value, returning `-6`/`-8` on failure.

So AF queries metering through a typed dispatcher rather than through a shared
buffer, which is why no AF code reads a frame: the numbers arrive as records.
This still does not show *what* is summed, and the 73 calls the other way mean the
two modules are coupled in both directions.

Statement of the round: call directions measured; the AF-consumes-metering
hypothesis is now supported but not proven.

## Round 47: 0xFF1302E8 is prop_deliver, and the 0xFF13 region is the property system

ML's own symbol table names 58 firmware addresses. One of them collides with a
claim made in round 4:

```
platform/6D.116/build/6D_116.sym:1404   ff1302e8 prop_deliver
```

So `0xFF1302E8` is **`prop_deliver`**, the property system's delivery routine --
not "the AF command function that takes a 24-byte command block". And the window
`0xFF130000`-`0xFF140000`, which rounds 4-8 called "the AF unit" and measured, is
the property system: its strings are `prop_savepropertydual`,
`prop_savepropertypartly`, `prop_savepropertymultipartly`, `pRegisterMaster`,
`pDeliverProperty`, `prop_testFactory`.

What the handler actually does, re-read with the right name:

```
ff3131dc  ldr  r0, [pc, #-3100]   @ global at 0xFF3125C8 -> RAM 0x0007B408
ff3131e4  ldr  r0, [r0, #24]      @ the property record
ff3131e8  mov  r2, #24            @ length
ff3131ec  ldr  r0, [r0, #72]      @ another property id
ff3131f0  b    0xFF1302E8         @ prop_deliver(id, buf, 24)
```

That is a **property mirror**: one property's change is delivered to a second
property id with a 24-byte payload. It is not a lens command, and there is no
"EmdDriveResult" in this path.

Withdrawn as a consequence:

* "the AF command chain: property -> handler -> 0xFF1302E8 (24-byte block) ->
  EmdDriveResult" (rounds 4-9) -- the middle step is property delivery;
* the measurements taken on `0xFF130000`-`0xFF140000` as if it were AF code.

Still standing: the dispatch table at `0xF83149DC` maps ids to handlers at
`0xFF31xxxx` (that is read out of the table, not inferred), and the actual AF
source path is `LiveViewAFPUtility.c` at `0xFF4324C4` (round 43).

The lesson repeats a third time in a different form: a function's *role* was
assumed from the handler that called it, when a symbol table in the repository
already had the answer. The earlier rounds never grepped the 58 `ff` addresses in
`6D_116.sym`; that grep takes a second and would have caught this in round 4.
