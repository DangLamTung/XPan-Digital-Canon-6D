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
