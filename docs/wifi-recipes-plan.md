# Fetching recipes over WiFi — plan, not built

Goal: pick a film simulation or a colour recipe on a phone, push it to the
camera, shoot. This document is the plan; **no part of it is implemented.**

## Correction: this body DOES have WiFi

An earlier version of this document said the 6D has no WiFi of its own. That
is wrong, and the camera's own firmware dump on the card says so. `strings` on
`ML/LOGS/ROM1.BIN`:

```
[NWCOM] Wlan Init Err
[NWCOM] NWCOM WLAN Biltin Judge (0x%x)
[NWCOM] WIFI_SETTING OFF
PROP_WIFI_SETTING (%d)
[FA_NW] FA_ChkWlan : %lx %lx
GuiPropertyData PROP_WFT_HTTP (%x)<-(%x)
SetHTTPUserName1<%s>
ComDLNAResizeSem
WlanMgr/WlanMgr.c  NWCommu/NWCommuTransOrder.c  NWMgr/NetworkSetting.c
Lan/DlgLnWirelessConnected.c  LanWizard/LnWizUtility.c
```

So the body carries WlanMgr, NWCommu, the LAN dialogs, DLNA and the WFT HTTP
server. Canon's WiFi is present; it is Canon's firmware that drives it.

## What ML can and cannot do with it on this body

| | state |
|---|---|
| Call Canon's WiFi stack from ML | **not today.** `platform/6D.116/build/6D_116.sym` has no `wlan_connect`, no `socket_create/send/recv` -- the symbols `modules/yolo` uses exist on 200D-class bodies, not here. `yolo.mo` is shipped in the build but cannot resolve those imports on this table |
| Add them | possible but large: they have to be found by disassembling the firmware (`ROM0.BIN`/`ROM1.BIN` are on the card) and added to the .sym. Legitimate work on your own firmware, zero guarantee it lands |
| Put files where Canon's app can see them | **works today.** Anything in `DCIM/nnnCANON/` is what the app lists and sends |

## Raw over WiFi

The transport is file-level over `DCIM`, and `.CR2` files sit in the same
folders as `.JPG`, so nothing in the firmware prevents moving a raw file. What
limits it is Canon's app and firmware policy, and **that is not verified** --
canon.com was not reachable from here and no authoritative statement was read.
The check costs two minutes on the camera: connect the app, open the file list,
see whether a `.CR2` is offered.

## Hazards, in the order they would bite

1. **A file written while the module is reading it.** `FIO_ReadFile` of a
   half-written `.fs` yields a parse that fails on `NAME` and the stock is
   skipped. The watcher must write-then-rename, or read only after two
   identical stat results — the same rule `fs_auto_task` already uses for new
   frames ("stable across two 2.5 s polls").
2. **Card filesystem caching.** The camera's FAT driver caches directory
   entries. A watcher must re-`FIO_OpenDir` each poll rather than trust a
   cached handle, and the log must print what it saw.
3. **Power.** A WiFi card draws continuously; on an idle camera that is the
   difference between a battery lasting hours and minutes. The watcher should
   poll at seconds, not continuously, and only while the module's screen is up
   or the camera is awake.
4. **No authentication.** These cards often serve an open AP. Any recipe
   dropped there is trusted and will be shot — the module already refuses a
   stock with no `NAME`, which is the only sanity check worth having.

## Plan, in order

### 1. Make a developed look reachable by the app (small, now)

Develop writes `IMG_nnnn_F.JPG`. `_F` is not a DCF name, so the camera's own
list -- and therefore the app's -- has no reason to show it. Give Develop an
option to write the DCF-legal `IMG_nnnn.JPG` that pairs with the CR2, which is
what "Replace Camera JPEG" already half does.

### 2. Recipe push (small, now)

Already works if the phone writes the card: `ML/FILM/*.fs` is read at boot.
The only gap is noticing a change without a reboot. `fs_watch_task` plus a host
test that drops files into a temp directory and asserts the loaded set follows
makes the camera side genuinely ready for a phone, and it is testable here
without a WiFi card.

### 3. Our own server on the camera (large, optional)

Add `wlan_connect` + the `socket_*` family to `6D_116.sym` from the firmware
dump, then do what `modules/yolo` does on its bodies: a small HTTP server in a
module, so a phone can pull developed JPEGs, push `.fs` recipes and read the log
without Canon's app. The only route to an app of our own, and the only one with
real risk.

### 4. Decide the app question first

- **Canon's app + files** -- no app to write, limited to what Canon lists.
- **Our server + a web page** -- needs 3.
- **SD relay** -- works today, no code, no app.

## What I would build first

`fs_watch_task` plus a host test that drops files into a temp directory and
asserts the loaded set follows — that makes the camera side genuinely ready for
a phone, and it is testable here without a WiFi card. Everything above it is
somebody else's transport.

## Reversal attempt, 2026-11-06 — what was tried and what it proved

Goal was a **verified address**, not a feature. It was not reached.

### Facts established (reproducible)

| finding | how |
|---|---|
| ROM0 = 8 MB at `0xF0000000`, ROM1 = 16 MB at `0xF8000000` | `platform/6D.116/consts.h`; dump sizes match exactly |
| Flash is also mapped at `0xFF000000` | 831,900 words in ROM1 aim at that window |
| A name table of 386 entries sits at file `0xC02D90` | longest run of consecutive name pointers; entries are module/task names (`WDT`, `A2BIF`, `ROMC`, `CardServiceMgr`, `CSMgrTask`, one `Unknown383`) |
| One name → function record exists | `{ptr "NWComState", 0, 0xFF2616C8, 0}`, and `0xFF2616E8` disassembles to `push {r4, lr}` … `pop {r4, pc}` |
| The ROM has **no** DryOS symbol table | `prop_deliver`, `task_create`, `take_semaphore` all absent from the strings |
| `socket_create` / `socket_send` do not exist by name | name-blob scan; `NWCommWrap_*` do exist |

ROM identity for reproducing any of this:
`ROM0.BIN md5 1543b9813c38003d89af666cc07255d7`, `ROM1.BIN md5 8e16eee40721ab0a07b3e15486434fa9`.

### Why it stopped

The plan's gate was: three `NWCommWrap_*` names each resolving to a function that
contains their own identifying strings. Only `NWComState` resolved. The other
`NWCommWrap_*` names have **no pointer anywhere in the image**, so they are unused
strings, not table entries — there is nothing to map.

`socket_create`/`socket_send` are not in the name list at all, so the `yolo`
requirement cannot be satisfied by name on this body. What exists is Canon's own
`NWCommWrap_*` (PTP-over-IP), whose entry points are not reachable by the
string-or-name method.

### What would be needed to continue

Code-first reversing: load ROM1 in Ghidra at `0xFF000000`, start from the WFT/PTP
command handler and follow the call graph to a transport write. No names, no
strings to anchor on. That is a project, not a task, and a wrong address in
`6D_116.sym` makes the module refuse to load (harmless) but a wrong address that
*does* resolve crashes on call.

### Unaffected by this

`fs_film_watch_task()` makes any `.fs` that reaches `ML/FILM` live without a
reboot, so recipe push already works for any transport that can write the card.

### The real blocker, found by asking "surely there is WiFi material?"

There is a great deal of it. ROM1 holds, among others:

```
0x261855  NWCOM] TransmissionChangeCBR(0x%x)
0x261879  NWCOM] NWCOM Init
0x261924  Wlan Init Err
0x262ca2  WLAN Biltin Judge (0x%x)
0x268454  WLAN module SDIO ERROR!
0x0e26f8  WFT (%#x <= %#x)
0x0facf2  WFT 0x%x INNER 0x%x
0x43840e  NWCommu/NWCommuTransOrder.c
0xccf0c5  NwTransferOrder          (an entry in the 386-name table)
```

Not one of them has a single pointer to it. Checked as an exact word, with a
-8..+64 tolerance, under four candidate bases (`0xFF000000`, `0xF8000000`,
`0xFF0C0000`, `0xF80C0000`) and three RAM bases (`0x40000000`/`0x41000000`/
`0x42000000` plus the string offset): **zero matches everywhere**.

So this firmware does not reach its log strings by literal or by `ADR`. Canon's
logging goes through a message-table macro with a base register, which leaves no
address in the image to find. That is why every string-anchored search in this
session returned nothing, `halt all sockets` included -- not because the code is
absent, but because the anchor does not exist.

### Where the code is, for a Ghidra session

The WiFi module's data is contiguous at file `0x26xxxx` (x10 the range
`0xFF200000`-`0xFF270000` under the alias base). Its code is the region
immediately before that. A Ghidra project on ROM1 at `0xFF000000`, starting in
`0xFF1E0000`-`0xFF270000`, is the shortest route that does not depend on strings.

## Round 30: step 1 is already implemented, and it is not "_F"

The plan above says "Develop writes `IMG_nnnn_F.JPG`, which is not a DCF name, so
give Develop an option to write the DCF-legal `IMG_nnnn.JPG`". The code no longer
does that. In `modules/film_sim/film_sim.c` the developed file is written to

```
DCIM/<dir>/IMG_<nnnn>.JPG        (snprintf at lines 1016, 7130, 7143)
```

with `film.sim.replace` defaulting to **1** (`CONFIG_INT("film.sim.replace",
film_sim_replace, 1)`), so the developed JPEG takes the number of the shot it came
from and **replaces the camera's own JPEG** at that name. The only `_F` left in
the file is in two comments describing an older measurement.

Consequence for the Wi-Fi question: the "works today" path in this plan is not a
to-do, it is the shipped behaviour. A phone running Canon's app should see a
normal `IMG_nnnn.JPG` that already contains the film-sim look, because from the
camera's point of view it is the same file it wrote. That needs no firmware
address, no NWCOM call and no switch id.

What still needs the camera, and is cheap:

1. with Wi-Fi on and the app connected, confirm the app lists `IMG_nnnn.JPG` and
   transfers it;
2. confirm whether a `.CR2` in the same folder is offered -- the question this
   plan has carried as "not verified" since 2026-11-06.

Both are observations, not code.

## Round 36-37: the file path checked line by line

The earlier round asserted that Develop already writes a Canon-visible JPEG. Here
is the chain, with the exact places it was checked:

| step | code | what it does |
|---|---|---|
| folder | `src/fio-ml.c:332` | `get_dcim_dir()` = `"%s:/DCIM/%03d%s"` from the shooting card's drive letter and **current folder number**, so output lands in the folder the camera is actually using |
| name | `modules/film_sim/film_sim.c:1016, 7130, 7143` | `"%s/IMG_%s.JPG"`, a DCF-legal name; the shot's own number is used when it is free, otherwise the next free number |
| replace | `film_sim.c:123` | `CONFIG_INT("film.sim.replace", film_sim_replace, 1)` -- by default the developed JPEG takes the camera's own JPEG slot |
| atomicity | `film_sim.c:7149-7165, 965` | written as `IMG_nnnn.TMP` and `FIO_RenameFile()`d to `.JPG` on success, with `fs_recover_tmp_files()` cleaning a `.TMP` at boot |

So the file Canon's app lists is a normal `IMG_nnnn.JPG` in the current
`DCIM/<folder>`, containing the film-sim look. No firmware address, no NWCOM
call, no switch id.

The one thing code cannot answer is whether Canon's app offers it: that is one
look on the camera (`docs/on-camera-tests.md`, Test 2).
