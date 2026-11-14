# 6D ROM: what the firmware holds for Wi-Fi and autofocus

Source of truth: `qemu/6D/ROM0.BIN` (8 MB at `0xF0000000`) and
`qemu/6D/ROM1.BIN` (16 MB at `0xF8000000`), plus the ML symbol lists and
`magiclantern/src/property.h`. Method and tool: `tools/rom/romscan.py`.

## Wi-Fi: the software is in the ROM, the radio is not on the board

| Evidence | Where |
|---|---|
| Marvell 88W8787 SDIO driver banner `w8787-Ax, RF878X, FP197, 14.197.9.p255, BT_SDIO` | ROM1 `0xF8CAEDF0` |
| `InitializeSDIODriver : sdioDrvCreateStateObject Error`, `sdioDrvExecAsyncFunction` | ROM1 `0xF82A01D8`, `0xF82A05D4` |
| `./SdioDrv/SdioDrv.c` — the SDIO transport unit | ROM1 `0xF82A02AC` |
| PTP over SDIO: `./PtpMgr/ComFrame/PTPRspnd/SdioPTPTrnsp2/` (6 units: bulk, event, HDD bulk/op, op, ctrl) | ROM1 |
| Socket stack: `halt all sockets`, `halt specified sockets`, `%d socket(s) halted.` | ROM1 `0xF8BA3B64` |
| WPA: `wpa version = %d`, `WIRELESS SETTINGS`, `[WLAN] CommandErr` | ROM1 |
| UI: `DlgLnWireless*.c`, `LiveViewWifiApp_handler`, `GetLvWLanStatusIcon` | ROM1 + `6D_116.sym` |
| Eye-Fi support (88 strings): `DlgMnEyeFiSetting.c`, `GetLvEyeFiOperatingStatusIcon` | ROM1 |

Reading: the firmware ships a complete WLAN stack — SDIO driver for the
88W8787, a TCP/IP socket layer, WPA, PTP over SDIO, and the menus to drive it.
The EOS 6D body itself has no Wi-Fi radio, so nothing in software can make a
radio appear; the only radios that can be reached are an **Eye-Fi card** (which
the firmware already supports) or an **SDIO Wi-Fi card in the card slot**.

Open questions for the next pass:
1. Does the WLAN init probe the SD slot for an SDIO device? `InitializeSDIODriver`
   is the entry to read.
2. Are the socket entry points reachable from a module? The ML symbol list has
   exactly one WLAN name (`LiveViewWifiApp_handler`), so the socket API is not
   exported and would have to be found by disassembly and called by address.

## Autofocus: the code units are identified

| Evidence | Where |
|---|---|
| `./AEWB/AF/AFProc/LiveViewAFPUtility.c` — the LiveView AF processing unit | ROM1 `0xF84324C4` |
| `./Gmt/GmtContinuousAf.c` — continuous AF (AI Servo) | ROM1 |
| `./LiveView/DlgLiveViewAfFrame.c`, `DlgLiveViewQuickAfFrameSetting.c` — the AF frame UI, `PROP_LV_AF_RESULT`, `PROP_AFPOINT` | ROM1 |
| `./Menu/DlgMnAfShiftSelectLens.c` — AF micro-adjust | ROM1 |
| `[TAG_AFMICROADJUST] failed` | ROM1 `0xF82EEE6F` |
| 195 strings mention AF points / frames / micro-adjustment; 178 mention focus | ROM1 |

### The properties that control AF (from ML's `property.h`)

| Property | Id | Notes |
|---|---|---|
| `PROP_AF_MODE` | `0x80000004` | 0 one-shot, 3 manual, 101 AI servo, 202 AI focus |
| `PROP_LVAF_MODE` | `0x80000050` | same value space, LiveView |
| `PROP_AFPOINT` | `0x8000000A` | 70-byte AF point block |
| `PROP_LV_AFFRAME` | `0x80050007` | AF frame in LiveView |
| `PROP_LV_AF_RESULT` | `0x80050029` | 0 = focus OK, 1 = failed; fired when AF ends |
| `PROP_AF_METHOD_SELECT_FOCUS_AREA` | `0x80000046` | focus-area method |
| `PROP_AFSHIFT_LVASSIST_STATUS/RESULT` | `0x8003003D/3E` | LV assist (focus shift) |

That is the control surface: mode, point, frame, result. The algorithm itself
(contrast evaluation, ramp, hysteresis) lives in `LiveViewAFPUtility.c` and the
`Gmt` continuous-AF unit, both without symbols, so reading it means
disassembling from the strings outward.

## Round 2: ROM1 holds real ARM code, ROM0 does not

Counting ARM patterns over both dumps:

| dump | `push {..,lr}` | `bl` | reading |
|---|---|---|---|
| ROM0 (8 MB, `0xF0000000`) | 0 | 30,964 | no function prologues: data / compressed |
| ROM1 (16 MB, `0xF8000000`) | 33,381 | 268,575 | **real ARM code** |

So ROM1 is the analysable image, and the earlier sample that looked like garbage
was a data island inside it.

### The AF properties are literals in ROM1 code

| property | literals found |
|---|---|
| `PROP_AF_MODE` `0x80000004` | 56 |
| `PROP_AFPOINT` `0x8000000A` | 26 |
| `PROP_LV_AFFRAME` `0x80050007` | 9 |
| `PROP_LV_AF_RESULT` `0x80050029` | 7 |
| `PROP_LVAF_MODE` `0x80000050` | 6 |
| `PROP_AF_METHOD_SELECT_FOCUS_AREA` `0x80000046` | 4 |

A dispatch table of property ids sits at `0xF83149DC` (`0x80050000`,
`0x80050002`, `0x80050005`, `0x8005000A`, `0x8005000B`, ...) with handler
addresses in the `0xFF31xxxx` range.

### A function that registers AF properties (annotated)

```
f83148e0  ldr   r1, [r5, #24]
f83148e4  ldr   r2, [pc, #324]      @ 0xf8314a30   (descriptor)
f83148e8  str   r0, [r1, #72]
f83148ec  add   r3, r4, #304
f83148f0  mov   r1, #0
f83148f4  stm   sp, {r1, r2, r3}
f83148fc  ldr   r0, [pc, #304]      @ 0xf8314a34   (property id)
f8314900  mov   r2, #31             @ length 31
f8314904  add   r1, r4, #316
f8314908  bl    0xf8311350          @ register / deliver
```

The same shape repeats with ids from the table and lengths 2 and 9 — that is the
handler-registration path for the properties around `0x80050000`.

## What is still missing, and why

Both the AF algorithm (`LiveViewAFPUtility.c`, `GmtContinuousAf.c`) and the
WLAN stack (88W8787 driver, sockets) live in the **`0xFF000000` region** (the
handler pointers above point there). None of the dumps in the workspace cover
it: ROM0 is `0xF0000000`, ROM1 is `0xF8000000`, and `film_eval/rom/RAM4.BIN`
(256 MB) is ML's own memory, not that code.

To finish either goal the next dump has to cover `0xFF000000`-`0xFFFFFFFF`
(16 MB). ML's Debug -> "Dump ROM and RAM" writes ROM0/ROM1; the IROM region
needs either that dump extended or a separate read through ML's memory browser.

## Round 3: the missing dump was an alias

ROM1 is mapped **twice**: the dump starts at `0xF8000000`, and the firmware's
own pointers use **`0xFF000000`**. Both name the same bytes, so an address in
`0xFF0xxxxx`-`0xFF3xxxxx` is at file offset `addr - 0xFF000000`. Verified by
disassembling handler `0xFF3131F4`: it decodes as a real function
(`push {r3, lr}`, byte loads, `bl`), not as data.

`tools/rom/romscan.py dis 0xFF000000,<start>,<end>` now disassembles that alias.

### AF property handlers, extracted from the table at `0xF83149DC`

| property ids | handler (`0xFF…`) |
|---|---|
| `0x80050000 0x80050002 0x80050005 0x8005000A` (0x0A = `PROP_AFPOINT`) | `0xFF3131F4` |
| `0x8005000B 0x8005000C 0x8005000D` | `0xFF313128` |
| `0x80050008` | `0xFF313164` |
| `0x80050010 0x8005001B 0x8005001E 0x80050027` | `0xFF3120A0` |
| **`0x80050029` = `PROP_LV_AF_RESULT`** | **`0xFF3131A0`** |
| `0x80050028` | `0xFF3131DC` |
| `0x8005002B` | `0xFF3132FC` |
| `0x80050033` | `0xFF313340` |

`PROP_LV_AF_RESULT` handler, in full:

```
ff3131a0  push {r2, r3, r4, lr}
ff3131a4  mov  r4, r0            @ payload
ff3131a8  mov  r2, #7            @ 7 bytes
ff3131ac  mov  r0, sp            @ to the stack
ff3131b0  bl   0x6bb98           @ copy
ff3131b4  mov  r0, #0
ff3131b8  strb r0, [sp, #6]      @ terminator
ff3131bc  ldr  r0, [r4, #4]
ff3131c4  and  r1, r0, #255
ff3131c8  ldr  r0, [r4]
ff3131d0  and  r0, r0, #255
ff3131d4  bl   0x5fb8            @ dispatch into low RAM
ff3131d8  pop  {r2, r3, r4, pc}
```

The AF point handler `0xFF3131F4` has the same shape with 3 bytes, and a
`PROP_AF_MODE`-style signature handler sits at `0xFF3131DC` (24-byte block,
reads `[r0+0x48]`).

### Wi-Fi: the socket layer is code, not just strings

The string `halt all sockets` has an alias literal at `0xFFBA3B64`, and it is
referenced from code at **`0xF84F3F60`**:

```
f84f3f7c  ldr  r0, [r5, r4, lsl #2]   @ socket slot
f84f3f80  bl   0xF833FECC
f84f3f88  mov  r2, r6
f84f3f8c  add  r0, pc, #76            @ format string
f84f3f90  bl   0xF80C1F28             @ log
f84f3f98  cmp  r4, r7
f84f3f9c  blt  0xF84F3F60             @ next socket
f84f3fa4  pop  {r4, r5, r6, r7, r8, pc}
```

So the WLAN stack's socket table and its logging are in ROM1 around
`0xF84F3F60`, and the SDIO/PTP transport is around `0xF82A0xxx`.

### What is proven, and what is not

Proven: the handlers exist at these addresses; the property→handler table is
readable; the socket code is present and calls into `0xF833FECC`; the SDIO
driver strings sit next to `InitializeSDIODriver`.

Not yet: the AF **algorithm** (the evaluation/ramp code) and the WLAN **init and
socket entry points** ML could call. Both need the code to be walked from the
handlers above, which is the next pass.

## Round 4: the AF handlers are marshals, and one of them forwards into the AF unit

Disassembled from the table in round 3:

```
ff3131dc  ldr  r0, [pc, #-3100]   @ global at 0xff3125c8
ff3131e4  ldr  r0, [r0, #24]
ff3131e8  mov  r2, #24            @ a 24-byte command block
ff3131ec  ldr  r0, [r0, #72]
ff3131f0  b    0xff1302e8         @ tail call into the AF unit
```

`0xFF1302E8` is a real function (prologue `push {r3-r9, sl, fp, lr}`, calls
`0xA5CC` for a lock and `0x1900` for logging, with two error strings at
`0xFF130288`/`0xFF13028C`). That is the AF command path: the property handler
forwards a 24-byte block to it.

The other two handlers are pure marshals into the property dispatcher at
`0x5FB8` (a low-RAM routine):

```
ff3131f4  push {r3, lr}      @ PROP_AFPOINT
ff3131f8  ldrb r2, [r1]      @ copy 2 bytes of the payload
ff313208  mov  r2, #3        @ 3-byte block on the stack
ff313224  and  r0, r0, #255
ff313228  bl   0x5fb8

ff3120a0  push {r3, lr}      @ AF group 0x80050010/1B/1E/27
ff3120a4  ldrb r1, [r1]
ff3120ac  mov  r2, #2        @ 2-byte block
ff3120cc  bl   0x5fb8
```

So the AF control surface is: property table (`0xF83149DC`) -> handler ->
`0x5FB8` (status notifications) or `0xFF1302E8` (AF commands). The evaluation
loop itself is not in these handlers; it is the task that consumes them.

## Wi-Fi: part of the socket code is beyond the dumped ROM

The halt-sockets loop calls out of the 16 MB image:

```
f84f3f60  ldr  r0, [r5, r4, lsl #2]   @ socket[i]
f84f3f64  bl   0xf833fecc
f84f3f68  bl   0xf90554f8             @ 0xF905xxxx -- past ROM1's end
f84f3f70  ...
f84f3f40  bl   0xf905555c
```

ROM1 covers `0xF8000000`-`0xF8FFFFFF` (16 MB), so `0xF90554F8` and
`0xF905555C` are outside it. ML's `consts.h` only knows ROM0 and ROM1, so the
dump that would finish the Wi-Fi analysis has to cover **`0xF9000000` and
above** -- that is the concrete next input needed, the same way the `0xFF`
alias was for the AF handlers.

## Round 5: the AF property family, and the ROM is bigger than the dump

Walking the descriptor table at `0xF8B6EB64` (16-byte entries: id, flags, size,
index) gives **30 AF-family properties**, and every one of them is a **9-byte**
payload except the last:

```
0x80050000 0x80050002 0x80050005 0x8005000A 0x8005000B 0x8005000C
0x8005000D 0x80050008 0x80050010 0x8005001B 0x8005001E 0x80050027
0x80050029 0x80050028 0x8005002B 0x80050033 0x80050035 0x8005003A
0x8005003B 0x8005003E 0x80050040 0x80050041 0x8005003D 0x8005003F
0x80050042 0x80050045   (0x80050045 is 14 bytes)
```

A 9-byte payload is one length byte plus two 4-byte values -- the shape the
marshalling handlers in round 4 copy onto the stack.

### The AF command chain, verified

```
property write
  -> handler 0xFF3131DC   (table 0xF83149DC)
  -> 0xFF1302E8           prologue push {r3-r9, sl, fp, lr}; takes a 24-byte block
       calls 0xFF130828   (8 times) -- thin wrapper around 0x39F54 that logs on error
       calls 0xA5CC       lock
       calls 0x6BB98      copy
       calls 0x90F4, 0x91AC, 0x904C   small RAM helpers
```

So the property layer only *queues* AF work; the evaluation loop is a task that
consumes it.

### The dumped ROM is not the whole ROM

Across ROM1 there are **2,681** 32-bit literals pointing into
`0xF9000000`-`0xF9FFFFFF` and **1,989** into `0xFA000000`-`0xFAFFFFFF` -- both
outside the 16 MB dump. The Wi-Fi socket code itself calls there:

```
f84f3f40  bl 0xf905555c
f84f3f68  bl 0xf90554f8
```

ML's `platform/6D.116/consts.h` declares `ROM1_ADDR 0xF8000000` and
`ROM1_SIZE 0x01000000`, and `src/debug.c` writes exactly that range to
`ML/LOGS/ROM1.BIN`. So the next dump has to extend to at least
`0xF8000000 + 0x02000000` before the Wi-Fi stack can be read, the same way the
`0xFF` alias had to be found before the AF handlers would decode.

## Round 6: the AF control surface ML already uses is the one found in the ROM

ML's own code and the ROM handlers line up exactly:

| ML call | property | ROM handler |
|---|---|---|
| `set_af_point()` (`src/af_patterns.c`) writes `PROP_AFPOINT` (`0x8000000A`) | 9-byte payload | `0xFF3131F4` (copies 2 bytes, dispatches to `0x5FB8`) |
| AF result consumers read `PROP_LV_AF_RESULT` (`0x80050029`) | 9-byte payload | `0xFF3131A0` |
| `lens_focus()` (`src/lens.c`) writes a focus-command property with a step in `-3..3` (`0x8000 - step` for negative) | drives the lens | not a property handler: the command path is `0xFF1302E8` |
| `set_afma()` (`src/afma.c`) writes the micro-adjust values | AFMA storage | handler group `0xFF3120A0` |

So the control half of goal 2 is not just located, it is already exercised by ML:
`af_patterns.c` moves the AF point, `lens.c` drives the lens in steps, `afma.c`
sets the per-lens offset -- and each of those lands on a handler whose address is
in the table from round 3.

A second table format also turned up: **(handler, property) pairs**, e.g. at
`0xF8313F28` `0xFFB6EAB8 / 0x8000004E`, at `0xF8313F70` `0xFF312884 /
0x80030001`. That table is what the property manager walks at registration.

### What remains for the algorithm

The evaluation loop is the AF controller task, not a property handler. The AF
state object is a single global pointer (`0x0007B408`, held at `0xFF3125C8`);
only three literals reference it directly, and two of those are tables, so the
controller reaches the state through another path. Reaching the loop needs
either:

1. a DryOS task-list walk (the task name strings `AfController`,
   `AfAeController`, `AfComplete` are in the ROM at `0xF824B2D4` and around),
   or
2. a live trace from ML: log the PC of the task that writes
   `PROP_LV_AF_RESULT` while a shot focuses.

## Round 7: the AF module's code is resident in RAM, and it can be disassembled

`film_eval/rom/RAM4.BIN` is not only ML's heap: between `0x03C000` and
`0x043000` there are **83 functions** with `push {..., lr}` prologues, and the
data next to them names AF callbacks:

```
0x03DF64  "AfWrDmacCompleteCBR"
0x03DF78  "AfWrDmacAbortCBR"
0x03DF90  "AfCompleteCBR"
0x03DFBC  "AfOverRunErrorCBR"
```

The function right after those names (`0x03DFD8`) is AF point handling:

```
3dfd8  push {r0-r9, sl, fp, lr}
3dfdc  sub  sp, sp, #180
3dff4  bl   0x12e78              @ state object
3dff8  ldr  r0, [r0, #72]        @ the same +0x48 offset the ROM handler uses
3e018  mov  r3, #128
3e01c  ldrb r2, [r2, r1]         @ a bitmask of AF points
3e028  cmp  r4, #6               @ at most 7 points in this loop
3e030  add  r6, r6, #20          @ 20-byte records
```

and a second one at `0x03D560` that calls into the AF unit and multiplies two
16-bit fields of a record:

```
3d5b0  bl   0xff1319c4           @ AF unit
3d5b8  ldrh r1, [r0, #70]
3d5bc  ldrh r0, [r0, #72]
3d5c0  mul  r1, r0, r1
3d5c8  bl   0xff4f9eb0
```

There are 20 `mul`/`mla` instructions in that block and no `smlal` pairs, so the
correlation-style math of a contrast AF is not concentrated here -- this block is
the point/state handling, not the evaluation. The evaluation still has to be
found, but the search space is now a 0x7000-byte RAM region with 83 functions
rather than a 16 MB image.

Property tables also live in RAM, e.g. at `0x4B2570`:

```
4b2564  0x80050002  flags 0  handler 0x0046B434
4b2570  0x80050029  flags 0  handler ...
4b254c  0x80030021  flags 0  handler 0x0046B574
```

The `0x0046xxxx` handlers are ML's own loaded module code, so this table is the
one the firmware fills from ML's registrations.
