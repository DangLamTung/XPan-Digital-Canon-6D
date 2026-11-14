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
