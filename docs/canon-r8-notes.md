# Canon R8: what lclevy/canon_fir and canon_basic establish

Read on 2026-11-17. Sources:
<https://github.com/lclevy/canon_fir>, <https://github.com/lclevy/cbasic_examples>,
slides `camera_jailbreak_v2.pdf` (TLP:AMBER version, in the canon_fir repo).

## FIR format generations

| body | FIR | note |
|---|---|---|
| 40D (2007) .. before EOS R | v4 | HMAC-SHA1 signature, AES, seeds from a dump |
| EOS R (2018, DIGIC 8) | v5 | same file shape, different key generation |
| R5/R6/R3 (DIGIC X) | v6 | same shape again |
| R1, R5 Mark II | v7 | container: magic `0x37582111`, count 3, 0x50-byte header, embeds v6 FIRs |

The R8 is DIGIC X, so it belongs to **v6**.

Crypto (documented, but **no keys**): AES-128-CTR payload, HMAC-SHA1 over the
firmware records, keys derived from `seed1`/`seed2` read out of a camera dump.
The readme is explicit: "No keys are published. You'll need a camera dump to
extract seeds and compute signing and decrypting keys."

So there is no path to a forged R8 FIR without a dump.

## The interesting part: Canon Basic runs without any signature

`cbasic_examples` documents Canon's embedded BASIC interpreter, which exists on
**DIGIC 8 and DIGIC X** bodies:

* the script file must be named **`extend.m`**
* the card is prepared as in the CHDK wiki "Canon Basic/Card Setup"; the easy way
  is Kitor's SD image (ML forum topic 25305, post 230427)
* on two-slot bodies (R6) the script goes on the **back card, `B:`**
* it can call firmware by address: `ExportToEventProcedure("FIO_ReadFile", addr|1)`
  then `cpuinfo(...)`, etc.

Ready-made, and validated on several DIGIC X bodies:

```
Universal/extend_dump_rom.m    -> SaveAllRomImageToFile()      full ROM dump, no addresses
Universal/extend_fw_sign.m     -> firmware checksum to B:/FW_SIGN.TXT
Universal/extend_bootdisk.m    -> enable/disable bootflag (boot magiclantern.bin from card)
R6.150/extend_cpuinfo.m        -> CPU info dump; R6.150 firmware addresses
```

DIGIC X firmware in those scripts lives at **`0xE0xxxxxx`** (ROM0 base
`0xE0100000`, ROM1 `0xF0000000`) -- a different map from the 6D's `0xF0`/`0xF8`.

## What this does and does not say about the R8

* Not tested: the repository has **no R8 directory** (only M100, M50, R.180,
  RP.160, R6.150 and Universal). Nothing here is validated on an R8.
* Plausible: the R8 is DIGIC X like the R6, and the universal scripts state they
  are validated on several DIGIC X bodies. The first thing to try is the ROM
  dump, which needs no addresses at all.
* The dump is also the missing piece for everything else: with ROM in hand the
  `FIO_*`/`umalloc` addresses for the R8 can be found, and only then can the
  R6.150-style scripts be ported.

## Risk

Canon Basic is an interpreter, so a wrong script usually ends in ERR70 or a
freeze and a battery pull, not a brick -- but the authors say the risk is never
zero, and the scripts here are other people's code.

## How ROM was read and bootflag enabled, generation by generation

From Laurent Clévy's slides (BeeRump 2022 / Hack.lu 2025, `camera_jailbreak_v2.pdf`)
and the Hack.lu 2025 abstract <https://pretalx.com/hack-lu-2025/talk/3UEDY8/>.

The slides carry one table, "CODE EXEC PATH PER DIGIC GENERATIONS", with rows
Cbasic / FIR / autoexec / ptp / uart:

| body | Cbasic | FIR | uart |
|---|---|---|---|
| 40D (d3), 5Dm2 (d4), 1300D (d4+), 5D3 (d5), 80D (d6), 5Dm4 (d6+), 6D2 (d7) | no | v4, forgeable | ok / yes |
| M50 (d8), EOS R (d8) | **ok** | v5, not forgeable | ok |
| R6 (dx) | **ok** | v6, not forgeable | ok |
| R10 (dx, 2022) | **locked** | v6 | **locked** |

So the historical routes in were:

1. **CBasic** -- `extend.m` on the card, `SaveAllRomImageToFile()` for the ROM and
   `extend_bootdisk.m` to set the bootflag. Works on DIGIC 8 and early DIGIC X
   (R, RP, R5, R6), locked on R10. The Hack.lu abstract states the same: "dumping
   the firmware ... is possible on EOS R, RP (digic 8) and EOS R5/R6 (digic 10)".
2. **UART** -- kitor enabled the bootflag on the EOS R over UART on 14/02/2019 and
   loaded a custom autoexec.bin payload to dump; the slides call this "not suited
   to allow everyone to dump and port ML".
3. **PTP event procedures** -- a custom PTP command, `0x9052`, can call an event
   procedure by name; Petabyte enabled the bootflag over PTP on the 1300D. Locked
   on M50 and later.

## Why the R8 is the hard case, in the words of the sources

* ML forum thread 27330 (June 2025): Bilal got "Hello, World!" on an R8, and kitor
  answered: "we don't know any good method to enable Magic Lantern (autoexec.bin /
  enable bootflag) to run on this model ... our old methods are now blocked (Basic
  script doesn't work anymore)".
* Since 2018 (EOS R) the FIR signature is **asymmetric**; the Hack.lu talk
  identifies it as ECDSA over secp256r1 for DIGIC 8 and DIGIC 10. The talk's own
  conclusion: "it will be not possible to forge valid signatures for recent FIR
  updates without the private key".
* `d8_oracle.py` decrypts DIGIC 8 updates by emulating the camera's own crypto from
  a dump, and `d810_verify.py` verifies signatures -- neither forges one.

So for an R8 there is, in the public record, **no** documented path: CBasic is
reported blocked, PTP event procedures are locked on DIGIC X, UART is listed as
locked from the R10 on, and the FIR route needs a private key.

## What that means if ML is running on an R8

The public record does not contain the answer, so the only honest statements are
the ones that can be checked on the body:

* if `extend.m` still executes, the R8's firmware build predates the lock, and the
  ROM came from `SaveAllRomImageToFile()` and the bootflag from `extend_bootdisk.m`;
* if a UART console answers in the bootloader, the ROM could be read there;
* otherwise the route is a vulnerability that is not documented in any of the
  sources read here, and it would postdate the November 2025 material.

## Tornado EOS: the bootflag and ROM read already exist commercially

<https://tornadosw.com/tornado-eos.php> -- "service software for Canon EOS
cameras", "full support of all 54 actual models". The model list includes **EOS
R8**, next to R7, R10, R50, R100, R3, R5, R6 and R6 Mark II.

Its feature list answers both halves of the R8 question:

```
Basic      Read and Write whole MPU EEPROM
           Read whole MPU Flash ROM
Extra      Read whole ICU Flash ROM's
           Read whole ICU Serial Flash
           Read and Set Mode: Norm/Fact/Service
           Update and Downgrade Firmware
           Set Update and BootDisk Flags          <-- the bootflag
```

So on an R8 the bootflag is not a research problem for a service tool: it is a
flag pair ("Update" and "BootDisk") in MPU EEPROM, written after the camera is put
into **Fact/Service mode**. What is not public is the wire protocol that switches
the mode and writes the flag.

What is known about the protocol:

* the tool talks to the body over USB, and the ML thread about it (topic 23241,
  2018-2019) shows a 6D owner using it to "dump 1024 Kb MPU as a bin file",
  unlock the language lock and toggle C-Log -- i.e. PTP-level service operations,
  not a firmware update;
* Greg's reply in the same thread notes the language lock is just property
  `0x01000012`, which suggests at least part of what Tornado does is ordinary
  Canon property access, and the rest needs the service mode;
* no public document describes the service-mode operations.

### How to get the protocol, concretely

1. run Tornado in a Windows VM with **USBPcap** and capture the session;
2. decode the capture with `tools/ptpcap.py` (written for this): it parses the
   USBPcap pcap and prints every PTP container with op code, parameters and
   transaction id;
3. compare that list with what `tools/ptp_probe.py --usb` reads from the same body
   in normal mode -- the difference is the service-mode surface.

`tools/ptp_probe.py` now has pyusb vendored under `tools/pylibs`, so `--usb`
works without touching the system Python.

Caveat: writing EEPROM flags blind on a DIGIC X body is how bodies get bricked.
Capture first, decode second, write never until the exact op and parameter set is
reproduced on a body you can afford to lose.

## MLinstall: the exact bootflag recipe, and why it stops at DIGIC X

<https://github.com/petabyt/mlinstall> (petabyt), master as of October 2026.

The install path is not a FIR update at all -- it is two PTP event procedures:

```
0x9050  EOS_EnableEventProc        sent up to three times
0x9052  EOS_ExecuteEventProc       data = packed procedure call
0x9053  EOS_GetEventProcReturnData
```

and the procedure it calls for the bootflag is, literally (src/mlinstall.h):

```c
#define ENABLE_BOOT_DISK  "EnableBootDisk"
#define DISABLE_BOOT_DISK "DisableBootDisk"
```

The payload is `canon_evproc_pack()`: the procedure name as a NUL-terminated
string, a u32 argument count, one 20-byte `{type, number, p3, p4, size}` record
per argument, then a footer. For `EnableBootDisk` with no arguments that is 23
bytes, `"EnableBootDisk\0" + u32 0 + u32 0`.

And the DIGIC X stop is this, in `ptp_eos_activate_command()`:

```c
// I will make baseless assumption that if IsNeoKabotanProcMod is available, then PTP is locked
// In that case, 0x9050 has different behavior.
if (ptp_check_opcode(r, PTP_OC_EOS_IsNeoKabotanProcMode)) {
    return PTP_RUNTIME_ERR;
}
```

which surfaces to the user as "The PTP unlock method (enable custom boot) is not
available on this camera. Sorry!". The R6 advertises `0x9060`, so MLinstall stops
there; no R8-specific work appears in the releases (latest 1.1.2, June 2025) or in
the commits up to October 2026 (card tab, drive backend, merging `canon.c`).

**So the R8 question is untested, not answered.** The tool never sends `0x9050`
on a body that has `0x9060`, and its own author calls the rule an assumption. That
leaves one cheap experiment nobody has published: on an R8, check whether
`0x9050`/`0x9052` are advertised at all, and if they are, whether
`ExecuteEventProc` still works.

`tools/r8_bootflag.py` does exactly that: `--check` sends nothing, `--evproc
FA_GetBuildVersion` runs a read-only procedure, `--enable` sends
`EnableBootDisk` (both need `--yes`).

## Tornado, priced (checked 2026-11-17)

From <https://tornadosw.com/tornado-eos.php>:

```
Tornado EOS   $40 / 5 days      $8/day   1 license, 1 PC, fully functional
Tornado EOS   $150 / 30 days    $5/day
Tornado SCF   free              shutter/mirror/error counters, serial,
                                sensor serial, mode Norm/Fact/Service,
                                language lock, C-Log, Update Firmware
```

Windows only, but "Emulators are supported (VirtualBox, VMWare, Parallels)";
online activation, telemetry, no refunds, one computer per licence. Payment via
Paddle (cards/PayPal/Apple Pay).

The paid tool is the only documented way to do the two things this project needs
on an R8:

```
Read whole MPU Flash ROM      -> the ROM dump the FIR key derivation needs
Set Update and BootDisk Flags -> the bootflag
Read and Write whole MPU EEPROM
Read and Set Mode: Norm/Fact/Service
```

So the practical answer to "how do they read the ROM and enable the bootflag on
an R8" is: a service centre spends $40 for a five-day licence and clicks two
buttons. The research problem is not that the operation is impossible, it is that
the protocol behind it is closed.

### What that is worth doing

1. **$40 / 5 days** gets a ROM dump and a bootflag on your own body, which is
   exactly the pair Magic Lantern needs to run on an R8 at all.
2. The same five days are the only clean opportunity to capture the protocol:
   run Tornado inside the Windows VM with USBPcap, and decode the capture with
   `tools/ptpcap.py`. That turns the $40 into a description we could reimplement.
3. The free SCF tool does "Update Firmware" -- worth checking whether it accepts
   an older, genuinely signed FIR, which would make a downgrade free.
