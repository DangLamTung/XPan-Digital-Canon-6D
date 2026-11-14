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
