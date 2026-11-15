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

A generated, always-current version of this table is `docs/af-property-map.txt`
(`tools/rom/afmap.py`, which reads the dispatch table, the descriptors and ML's
`property.h` in one pass).

The AF property family has **30 descriptors** in the table at `0xF8B6EB64`
(16-byte entries: id, flags, size, index), and all of them are **9-byte**
payloads except `0x80050045`, which is 14.
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
