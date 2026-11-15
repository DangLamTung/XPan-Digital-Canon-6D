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
4. **AEWB touches peripherals**: `0xC0080000`, `0xC0080500`, `0xC0080700`,
   `0xC0E00000`, `0xC0E2008C` appear as literals in that region, and the trace
   string `AEWB_AEL_SET_AFAE:%X Bv:%X` describes the firmware *publishing* an
   AE/AF value it was given.

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
