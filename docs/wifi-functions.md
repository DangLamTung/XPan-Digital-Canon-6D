# The 6D's WiFi: what was found, and how far it can go

Investigation notes, 2026-11-06. Everything here is from the firmware on the card,
not from documentation or memory.

## Identity of the images

| image | size | load address | md5 |
|---|---|---|---|
| `ROM0.BIN` | 8,388,608 | `0xF0000000` | `1543b9813c38003d89af666cc07255d7` |
| `ROM1.BIN` | 16,777,216 | `0xF8000000` | `8e16eee40721ab0a07b3e15486434fa9` |
| `RAM4.BIN` | 268,435,456 | unknown | — |

The load addresses come from `platform/6D.116/consts.h`, which is also what
`src/debug.c` dumps from. The same 16 MB of flash is **also mapped at
`0xFF000000`** — 831,900 words inside ROM1 point into that window — so all
disassembly below uses `0xFF000000` as the base.

## Why string search does not work on this firmware

Every WiFi message is a **format string loaded PC-relative** and passed to Canon's
logging routine at `0x67C8`:

```
ff2616f8:  add   r2, pc, #340    @ the format string
ff2616fc:  mov   r1, #3          @ level
ff261700:  mov   r0, #0x45       @ subsystem id
ff261704:  bl    0x67c8          @ log(subsystem, level, fmt)
```

Consequences, all tested:

- no absolute pointer to any WiFi string exists — checked exact and with a −8..+64
  window, under `0xFF000000`, `0xF8000000`, `0xFF0C0000`, `0xF80C0000`
- no `ADR` match either, because the strings are reached through a base register
  or a message table, not a direct `add rd, pc, #imm`
- inverting the RAM dump (any word minus the string offset landing on a 1 MB
  boundary in `0x40000000`–`0xAFFFFFFF`) returns nothing
- 16-bit offset tables: no dense run

So the anchor has to be **code**, not strings. Ghidra was used for that:
`ROM1.BIN` imported as `ARM:LE:32:v5` at `0xFF000000`, full analysis.

## The WiFi functions

299 distinct functions in `0xFF1E0000`–`0xFF270000` log identifiably. The spine:

| address | identity | evidence |
|---|---|---|
| `0xFF261714` | **`NWCOM Init`** — brings up network comms, allocates heap and BSS | `[NWCOM] NWCOM Init`, `pTopHeap`, `BssRegionSize`, `NetSetup Err` |
| `0xFF2616E8` | transmission callback | `[NWCOM] TransmissionChangeCBR(0x%x)` |
| `0xFF261B00` | network status callback | `[NWCOM] NetworkStatusCBR(Status=%d)` |
| `0xFF261F70` | device identity and discovery | `GUID:%x%x…`, `FriendlyName:%s`, `WizardProperty`, `Connect Confirm Judge` |
| `0xFF262358` | transfer order handler | `Nothing Order`, `FTP Transfer Stop Discover`, `FTP Transfer Count Non Zero(%d)` |
| `0xFF261974` | FTP transfer control | `FTP Transfer ReStart`, `From FTP Cancel Time out`, status tuple |
| `0xFF2634B8` | logical connect state | `DisConnectSuccess(0x%x)(Clr=0x%x)`, `ChangeLogicalConnect(0x%x)` |
| `0xFF2638C4` | connection monitoring and reconnect | `BeaconLoss!!`, `C2C ConnectCancel`, `WlanDisconnect -> ReConnect!!` |
| `0xFF266334` | GUI event fan-out | `NWCOM GUIEvent(0x%x)`, `Connect Event`, `DisConnect Event` |

Supporting strings in the same region, all unreferenced by pointer:
`WLAN module SDIO OK!!`, `Wlan Init Err`, `WLAN Biltin Judge (0x%x)`,
`NWCommu/NWCommuTransOrder.c`, `WFT (%#x <= %#x)`, `PROP_WFT_HTTP`.

## How capable is it?

Assessed strictly from what the strings and structure show.

**Present and complete:**

- a full **FTP** client path (start, stop, restart, cancel, timeout, progress)
- **device discovery** with GUID and friendly name — DLNA/UPnP-shaped
- **connection lifecycle**: associate, disconnect, beacon loss, auto-reconnect
- **C2C** (camera-to-camera) send and receive dialogs
- a **GUI event** layer that tells Canon's UI what the network is doing
- **WFT HTTP** — the web/transfer protocol Canon's own software speaks

**Not present, and this matters:**

- no `socket_create` / `socket_send` symbols anywhere in the image, by name
- no exported symbol table at all (`prop_deliver`, `task_create`,
  `take_semaphore` are all absent), so nothing can be resolved by name
- the transport is **Canon's own PTP/FTP stack**, not BSD sockets

**What that means for a module:** ML can only call what `6D_116.sym` exports,
and none of this is exported. Reaching it means calling fixed addresses directly.
That is possible — the addresses are now known — but it is a different kind of
risk from calling a named API: no signatures, no ABI guarantee, and a wrong call
crashes.

## Running the rung-1 probe

`modules/mlwifi/mlwifi.c` is the only code in this repository that touches any of
this. `Probe WiFi` reads the first word of each of the nine addresses and reports
whether it looks like an ARM prologue; it calls nothing. `Call NWCOM Init` is
rung 2 and is gated — see below.

It is in `platform/6D.116/modules.included`, so the zip carries it: install the
normal `magiclantern.zip` and it is already in `ML/modules/`. Without a build,
copy the `.mo` by hand instead:

```sh
cp magiclantern/modules/mlwifi/build/mlwifi.mo /Volumes/EOS_DIGITAL/ML/modules/
```

Then on the body: **Modules → mlwifi → Enable**, power cycle, **WiFi → Probe
WiFi**.

`WiFi → Call NWCOM Init` jumps to `0xFF261714` with r0–r3 = 0 and logs the
return. It refuses to run unless `allow_call` is 1 (**Modules → mlwifi →
allow_call**, default 0), because the signature is inferred from disassembly
rather than known. That gate is the reason the module is safe to ship: with the
default config it cannot touch firmware beyond reading nine prologues.

What the log should say, and what the answer means:

| log | meaning |
|---|---|
| `probe: 9 of 9 look like functions` | every address in the table is a real function entry; the next rung is worth trying |
| `probe: N of 9 ...` with `NOT a prologue` | that address is data, a Thumb function, or a mis-decoded instruction boundary. The table in this file needs fixing before anything is called |
| no line at all | the module did not load — check `ML/LOGS` for a load error, or that it is enabled |

Rung 1 is not a connection and this document does not claim one. Until a rung-2
call returns with Canon's own `[NWCOM]` lines in the log, the honest status of
WiFi in this project is **addresses found, stack not driven**.

## The property bridge (rung 1.5)

`PropMgr` is the one documented way into the WiFi stack, because ML exports
`prop_request_change`, `prop_deliver` and `prop_register_slave`, and because
`src/powersave.c` and `src/vram.c` already watch `PROP_LOGICAL_CONNECT` on this
build — the plumbing is proven.

The ids came from ROM1 at **`0xFF284FA0`**: a dispatcher that routes an incoming
property id to one of two handlers and tests exactly four values:

```
ff284fcc:  ldrne r0, [pc, #484]      @ 0x80030016
ff284fe0:  ldr   r0, [pc, #468]      @ 0x8003000F
ff284fe8:  subne r0, r0, #1          @     0x8003000E
```
plus `r5 == 0x80010000` from `add r0, r5, #-0x5000000` / `subs r0, r0, #0x10000`.

| id | note |
|---|---|
| `0x8003000E` | is ML's own `PROP_LOGICAL_CONNECT` (property.h:644) — independent confirmation that this block is the network properties |
| `0x8003000F` | tested by the same dispatcher |
| `0x80030016` | tested by the same dispatcher |
| `0x80010000` | tested by the same dispatcher |

`WiFi → Watch Props 60s` (Phase 1) watches the **whole block**
`0x80030000`–`0x8003003F` plus `0x80010000` — 65 ids — because the on/off switch
has not been identified. It prints the non-zero values once at the start, then a
line per transition for a minute:

```
watch: begin, 65 ids, block 0x80030000..0x8003003f + 0x80010000
state: 0x8003000e = 0
change: 0x8003003a 0 -> 1
```

Turn WiFi on and off in Canon's menu while it runs: the `change:` line names the
switch and its two values. That is what an on/off action has to write.

Implementation notes, because they are easy to get wrong:

- each id is a separate `PROP_INT` registered through the module's own property
  table. A single `prop_register_slave()` call with a runtime list would be
  tidier, but `prop_unregister_slave` is **not exported**, so a module unload
  would leave DryOS calling a freed handler. ML only unregisters handlers that
  came through `prop_add_handler` (`module.c:854`).
- `PROP_INT` only assigns. Nothing logs or sleeps inside `PropMgrTask`.
- nothing in this path writes a property or calls a firmware address.

What the functions are is *not* yet known — only that these four ids are the
ones this component cares about.

### Which name is which id

The log sites name their own property, so the ids can be tied to names by
reading the code that logs them:

| id | name | evidence |
|---|---|---|
| `0x8003000E` | `PROP_LOGICAL_CONNECT` | matches `property.h:644`; it is one of the two routed to `0xFF284EF0`, whose other case logs `PROP_LOGICAL_CONNECT_WFT[%x]` at `0xFF285148` |
| `0x80030016` | `PROP_CONNECT_TARGET_INNER` / `PROP_CONNECT_TARGET_WFT` | code at `0xFF279124` logs `PROP_CONNECT_TARGET_INNER (%#x <= %#x)` while comparing `r5` with `[r6+0x1c]`; **bit `0x40` of the value means connected** (`tst r5,#0x40` / `and r0,r0,#0x40` at `0xFF279144`) |
| `0x8003000F` | most likely `PROP_LOGICAL_CONNECT_WFT` | routed together with `0x0E` at `0xFF284EF0`; **inferred, not proven** |
| `0x80010000` | `PROP_INNER_PHYSICAL_CONNECT` or `PROP_TUNING_FLAG` | first branch of the dispatcher; **which of the two is unknown** |

### The string table at 0xFF284E18

A contiguous table, not code:

```
PROP_WFT_SYSTEM[%x]            Proribit USB SLOT1:%lx
PROP_PHYSICAL_CONNECT[%x][%x][%x]  Proribit USB SLOT2:%lx
Proribit USB TP/IP:%lx         Proribit USB HTTP:%lx
Proribit USB C2C:%lx           Proribit USB WLAN:%lx
```

Two things follow. **The firmware refuses WLAN while a USB path is active** —
"Proribit USB WLAN" is a deliberate refusal, not a bug, which is why WiFi cannot
be turned on with the cable plugged in. And this table is why pointer search
fails: no literal and no `ADR` anywhere in the image points into it, so it is
reached as *base register + immediate*; the index scheme is still unresolved.

### Not the camera's WiFi: the EyeFi flags

`GUI_SetEyeFiWiFiEnable` (`0xFF51193C`) is `str r4, [r0, #8]` where `r0` is the
literal `0x00083BFC` — a RAM struct with three flags read at `+0`, `+4`, `+8`.
Its neighbours are `EyeFiDataStorage:%d` and `EYEFI_PROPERTY_OPERATION_STATUS:%d`.
That is the **EyeFi SD-card** feature, not the 6D's built-in WiFi. Writing it
would do nothing for the radio.

## Signatures recovered so far

Only what the instructions actually show. Everything else is still unknown.

### `GUI_SetInfraESSID` — `0xFF18F21C` (connect: SSID)

```
ff18f21c: push {r4, lr}
ff18f220: mov  r4, r0            @ r0 = SSID string
ff18f224: bl   0xff0c8c6c        @ strlen
ff18f228: add  r0, r0, #1
ff18f22c: cmp  r0, #36           @ reject > 35 chars + NUL
ff18f230: pophi {r4, lr}
ff18f234: addhi r2, pc, #0xb8    @ "***** GUI_SetInfraESSID Error!!"
```

`void GUI_SetInfraESSID(const char *ssid)` — one pointer, nothing else. This is
the most confidence of anything in this file: the length check is right there.

Three callers, two behaviours:

| caller | what it passes |
|---|---|
| `0xFF721130` | a compiled-in literal — the factory SSID **`EOS-canon`** (at `0xFF7211E8`) |
| `0xFF7C4DD4` | a string buffer from the dialog context (`r5`), only when a state getter at `0xFF393FD8` does **not** return 3 |
| `0xFF721158` | the same state check, other branch (`GUI_SetAdhocCameraESSID` at `0xFF18F340`) |

So SSID setting is reachable and understood. **Join/associate is not** — no
function has been identified that takes that SSID and actually starts the
802.11 join; the `[WLAN] Infra Join OK` site is a *result* handler, not a
request.

### FTP transfer entry — `0xFF26D468`

```
ff26d468: push {r4,r5,r6,r7,r8,r9,sl,fp,lr}
ff26d46c: mov  r5, r0            @ arg1 = context struct
ff26d470: mov  r6, r1            @ arg2 = handle / flags
ff26d478: mov  r4, r2            @ arg3 = buffer
ff26d47c: mov  r1, #156          @ memset(arg3, 0, 156)
ff26d480: mov  r0, r2
ff26d484: bl   0x6be64
ff26d488: mov  r1, #128          @ memset(sp+76, 0, 128)
...
ff26d494: ldr  r0, [r5, #16]     @ arg1 is a struct of >= 0x18 bytes
ff26d498: ldm  r5, {r8, r9}
ff26d49c: ldr  r7, [r5, #8]
ff26d4a0: ldr  sl, [r5, #12]
ff26d4a8: ldr  fp, [r5, #20]
```

`int f(struct ctx *a /* 0x18 B */, int mode, void *buf /* 156 B */)`

**Phase D resolved the ctx.** The first caller zeroes it explicitly:

```
ff26d750: mov  r1, #24        @ ctx size = 24 bytes
ff26d754: add  r0, sp, #164   @ ctx base
ff26d75c: bl   0x6be64        @ memset(ctx, 0, 24)
```

and then fills six words at `+0, +4, +8, +12, +16, +20` — exactly the offsets
the entry itself reads (`ldm r5,{r8,r9}`, `[r5,#8]`, `[r5,#12]`, `[r5,#16]`,
`[r5,#20]`). Two independent functions agree on a 24-byte, six-word structure,
which is why this is called resolved rather than guessed.

The middle argument is a **mode**, and the three callers pass all three values:

| caller | mode | fields filled |
|---|---|---|
| `0xFF26D794` | `2` | `ctx+0x10`, `ctx+0x14` |
| `0xFF26E74C` | `0` | whole ctx, from the caller's own arguments |
| `0xFF26E7C4` | `1` | whole ctx |

The context is a transfer-channel descriptor being torn down — the log line at
the mode-0 caller reads `Release USB Size:%lx,%lx`, so the mode selects which
kind of channel (USB / WLAN / other) is being released.

What is still missing is the **meaning of the six words**. They come from the
caller's arguments, which come from the state machine above, so the struct is
sized and laid out but its contents are unlabelled. Callable in shape, not in
meaning — still not something to run.

### The request entry — `0xFF85959C` (Phase A result)

This is the closest thing to a public API in the whole stack, and it validates
its own arguments before touching anything, which is why it is the safest
starting point.

```
ff85959c: push {r4,r5,r6,r7,r8,lr}
ff8595a0: ldr  r3, [pc, #300]   @ 0xFF8596D4 -> 0x00009001
ff8595a8: cmp  r0, r3           @ arg1 must be 0x9001
ff8595ec: cmp  r8, #264         @ arg3 must be 264
ff859624: ldr  r0, [r7, #4]     @ arg2 + 4 -> command word
ff859628: sub  r1, r0, #0x7000
ff85962c: subs r1, r1, #0x21    @ compare against 0x7021
ff8597ac: add  r0, sp, #24
ff8597b0: bl   0xff866c7c       @ copy(dst, arg2, 264)
```

Recovered shape:

```c
int request(uint32_t magic /* = 0x9001 */,
            void    *block,  /* 264-byte request block */
            uint32_t size);  /* = 264, enforced */
```
with the **command word at `block + 4`**. The first command seen is `0x7021`:
that branch copies the block, then reads six bytes at `block + 32..37` and
passes them on with length 6 — the shape of a MAC address.

Every argument check was re-read back out of the image after being written down:

| instruction | expected | verified |
|---|---|---|
| `0xFF85959C` entry | `E92D41F0` | ok |
| `0xFF8596D4` magic | `00009001` | ok |
| `0xFF8595EC` size | `E3580F42` (264) | ok |
| `0xFF859624` cmd load | `E5970004` | ok |
| `0xFF85962C` cmd compare | `E2511021` (0x7021) | ok |
| `0xFF8597B0` copy call | `EB003531` | ok |

Still unknown at this point: the full command→name table (only `0x7021` is tied
to behaviour so far), and the layout of the 264-byte block beyond offset `+4`
and the MAC bytes at `+32`.

Callers of `0xFF85959C` found by `BL` scan: **none**, and the search was pushed
harder:

| probe | result |
|---|---|
| `BL` scan over the whole 16 MB image | 0 callers |
| literal of `0xFF85959C` | **exactly 1**, at `0xFF859A30` |
| context of that literal | a descriptor `{0x00000236, 0x00007021, 0xFF85959C, 0x0008F0E0}` — msgid, command, handler, plus one unknown word |
| literals / `ADD pc` to the descriptor (`0xFF859A28..0xFF859A34`) | 0 references |
| all six literals of `0x9001` | three sit immediately before `[WLAN] wwUiRequest(...)` strings, i.e. they are the **module id**, not a per-call magic |

So the entry is a registered handler for command `0x7021` in module `0x9001`,
and **the registration is not statically linked in this image** — the consumer of
that descriptor is created at run time. That is a hard stop for static analysis:
the next step is dynamic, not more disassembly.

## The FTP implementation, decoded

The transfer path is a **real FTP client**: `PASV`, `RETR`, `STOR`, `NLST`,
`PORT %d,%d,%d,%d,%d,%d`, plus `LIST`, `TYPE`, `QUIT` elsewhere in the image.
None of it is exported, so everything below was read out of the instructions.

### The socket API, by argument layout

These are ordinary BSD calls at fixed addresses. Not in `6D_116.sym`, but each
signature is visible in how the caller sets up the registers.

| address | function | evidence |
|---|---|---|
| `0x54414` | `socket(af, type, proto)` | `ldrh r0,[r4,#6]; mov r1,#1(SOCK_STREAM); mov r2,#0` at `0xFF766480` |
| `0x54604` | `bind(sock, addr, len)` | `mov r2,#8; add r1,sp,#28; mov r0,r5` at `0xFF76666C` |
| `0x5467C` | `listen(sock, backlog)` | `mov r1,#1` at `0xFF7666B4` |
| `0x546F8` | `connect(sock, addr, len)` | two sites: `(r5, r4+8, 8)` at `0xFF766514`, `([r4+0x24], sp+16, 8)` at `0xFF7668EC` |
| `0x547B0` | `getsockopt(sock, &opt, &len)` | `r1=sp+12; r2=sp+20` at `0xFF766BE0` |
| `0x549B8` | `send(sock, buf, len, flags)` | `([r4+0x20], sp+4, 320, 0)` at `0xFF7662AC` |
| `0x54C68` | `recv(sock, buf, len, flags)` | `([r4+0x20], sp+4, len, 0)` at `0xFF766434` |
| `0x5512C` | unresolved | five-argument shape with a pointer at `sp+4`; possibly a wait/select. **Not named.** |

### Session structure (from `r4` accesses, region `0xFF764E00`–`0xFF767000`)

| offset | type | meaning | evidence |
|---|---|---|---|
| `+0x00` | ptr | context | `ldr r3,[r4]` |
| `+0x04` | u16 | port | `ldrh r1,[r4,#4]`, stored into a sockaddr |
| `+0x06` | u16 | address family | `ldrh r0,[r4,#6]` is `socket()`'s first argument |
| `+0x08` | 8 B | `sockaddr` | passed whole to `connect()` |
| `+0x12`–`+0x18` | bytes | address text / pieces | byte-wise accesses |
| `+0x20` | handle | **control socket** | only ever `send`/`recv` |
| `+0x24` | handle | **data socket** | `connect()`, `getsockopt()`, closed on error |
| `+0x28` | u8 | state byte | `strb r9,[r4,#40]` |

`+0x20` vs `+0x24` is settled by use, not by naming: `+0x20` only ever appears as
the first argument of `send`/`recv`, `+0x24` only as the first argument of
`connect`/`getsockopt`.

### Command layer

`0xFF7660D4` builds the data-connection command from an enum:

```
ff766908: cmp  r8, #1      @ 1 -> RETR   (addeq r1, pc, #0xb4)
ff766914: cmp  r8, #2      @ 2 -> STOR   (addeq r1, pc, #0xb0)
ff766920: cmp  r8, #3      @ 3 -> NLST   (add r1, pc, #0xa8)
ff766930: bl   0xff33fed8  @ snprintf-style into sp+32
ff76694c: bl   0xff7663e4  @ send the command
```

`PASV` is not part of this switch — it is sent from its own site, and an
earlier note in this file claiming it was unreferenced was **wrong**:

| site | sends | gate | arguments |
|---|---|---|---|
| `0xFF7666FC` | `PORT %d,%d,%d,%d,%d,%d` | not the `[r4+6] == 1` case | six values taken from **session `+0x12..+0x15`** — the four IPv4 bytes — plus the port |
| `0xFF76680C` | `PASV` | `ldrh r0,[r4,#6]; cmp r0,#1` at `0xFF7667F8` | none |
| `0xFF76694C` | `"%s %s"` | extra argument present (`r9 != 0`) | the built command + one more |
| `0xFF766970` | `"%s"` | no extra argument | the built command |

So the active/passive choice is a branch on `[session+0x06]`, and the `PORT`
argument order confirms `+0x12..+0x15` are the IPv4 address bytes:
`ldrb r2,[r4,#18]; ldrb r3,[r4,#21]; ldrb r2,[r4,#20]` feed `%d,%d,%d,%d`.

An `add pc` decoding bug nearly hid this: rotating a 12-bit immediate instead
of an 8-bit one is only wrong when the rotate field is non-zero, and `PASV` sits
behind exactly such an instruction. Corrected and re-verified — all four targets
above resolve to real strings.

### Reply parsing

| address | what | evidence |
|---|---|---|
| `0xFF766240` | reply-code validator: first three characters must be digits | character-class table at `0xFF766574`, `tst r2,#4`; failure returns `-13` (`mvn r0,#13`) |
| `0xFF7661F4` | dotted-quad parser | counts `'.'` (`cmp ip,#3`), `sub r4,r3,#0x30` per digit, then calls `0xFF657E08` |

### Still not proven

- the **ordered** exchange for an upload. `PASV`/`PORT` now have sites and
  `STOR`/`RETR`/`NLST` are known, but `USER`/`PASS`/`TYPE` are not yet tied to
  call sites, so the order above the data connection is still an inference.
- whether `[session+0x06]` is really an address family. It is passed as
  `socket()`'s first argument **and** used as the passive/active switch, which
  is contradictory for a plain `AF_INET`. Unresolved.
- whether `0x5512C` is a wait/select.
- the six `ctx` words of `0xFF26D468` (Phase D of the FTP plan).

### Parameter builders — statically unreachable

`WICAP_SET_WIRELESS_PARAM`, `WICAP_SET_SECURITY_PARAM` and `WICAP_START_BSS`
exist as log strings but nothing in the image references them:

| probe | result |
|---|---|
| literal of each string address | 0 |
| `ADD rd, pc, #imm` resolving to each | 0 |

Same story as the string table at `0xFF284E18`: these live in a region reached
as *base register + immediate index*, and the index scheme has not been
recovered. So the SSID/key/channel structures behind "start an AP" cannot be
sized from here by this method.

### Not found at all

- WLAN join / scan request with a usable entry point (`NT_*` names exist only as
  log labels; no numeric command table recovered)
- any default WPA passphrase (the SSID is compiled in, the key is not — it is
  read through `GUI_Network_SmartPhone_GetEncryptionKey`, unknown signature)

## Realistic capability ladder

| rung | what | risk |
|---|---|---|
| 1 | log that a WiFi address is reachable (no call) | none — the module either loads or does not |
| 2 | call `NWCOM Init` and log the return | low — it is documented by its own strings as the bring-up routine |
| 3 | read network status via `0xFF261B00` | medium — argument shape unknown |
| 4 | drive the FTP transfer path to push a file | high — needs the whole call graph |
| 5 | push a `.fs` recipe through it | as 4, plus the card write on the far side |

Rung 4 is what "transfer data like JPG" means. It is not reachable by guessing;
it needs `0xFF262358` (transfer order) and `0xFF261974` (FTP control) disassembled
and their arguments reconstructed.

## Already true today, without any of this

- **Photos out over WiFi**: Canon's own WiFi GUI and app do it. It is in the
  camera and works.
- **Recipes in**: `fs_film_watch_task()` in `film_sim` picks up any `.fs` that
  appears in `ML/FILM` within about six seconds, no reboot. Any transport that can
  write the card — card reader, phone with an SD slot, writable share — is enough.

So the WiFi reversal only removes the card from the recipe step. It does not add
a capability that is entirely missing; it removes a walk to a computer.
