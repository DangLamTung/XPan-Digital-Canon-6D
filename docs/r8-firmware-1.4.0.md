# EOSR8140.FIR: what the R8 1.4.0 update actually contains

File: `eosr8-v140-win.zip` (78,124,992 bytes, downloaded 2026-11-17) containing
`EOSR8140.FIR`, 73,043,184 bytes (`0x45A8CF0`), dated 2024-07-17.

## The plaintext header

```
+0x00  80000487        model id -- the R8 (R6 reports 80000453)
+0x10  "1.4.0.5.6.9"   external 1.4.0, internal 5.6.9
+0x20  d4e10f33        unknown
+0x24  00000100        256
+0x28  00000120        288
+0x2c  ffffffff
+0x30  004b8b60        section boundary
+0x34  ffffffff
+0x38  045a8cf0        = file size
+0x4c  00000020        32
+0x50  00000024        36
+0x54  00000044        68
+0x58  00000100        256
+0x5c  004b8a60        section boundary
+0x60  004b8b60        section boundary
+0x64  040f0190        section boundary (near the end: 0x45a8cf0 - 0x40f0190 = 0x4b8b60)
+0x68  20 + 32 bytes   blob 1
+0x8c  20 + 32 bytes   blob 2
+0xb0  20 + 32 bytes   blob 3
+0xd4  20 + 32 bytes   blob 4
+0x100 004b8a40, 004b8a3c, then 16 more bytes, then 32 bytes at 0x120 ...
```

Four length-prefixed 32-byte values are what an ECDSA P-256 signature pair looks
like when split into r and s and stored twice (updater signature + firmware
signature), which is what lclevy's Hack.lu talk describes for DIGIC 8/10.

## Everything else is ciphertext

Shannon entropy per region:

```
header   0x000000..0x000100   5.51 bits/byte
body     0x000100..0x000500   7.96
+0x4b8a60  (4 KB)             7.95
+0x4b8b60  (4 KB)             7.95
+0x40f0190 (4 KB)             7.96
```

The only ASCII string in the first kilobyte is `1.4.0.5.6.9`. There is no
plaintext code section anywhere in the 73 MB: at 8.00 bits/byte the whole body is
AES-128-CTR ciphertext, the same as the 6D's v4 file (7.95).

## So what does it do?

Functionally, from the format (and lclevy's description of the update flow), an
update is: check the ECDSA signature, decrypt the updater, run it, write the new
firmware into flash, reboot. The *content* -- which modules changed between 1.4.0
and earlier versions, what code runs -- is not readable without the decryption
key, which is derived from seeds inside a camera dump.

## Can it be Ghidra'd?

No. Ghidra is installed on this machine (`/opt/homebrew/bin/ghidraRun`), but there
is nothing to disassemble: a decompiler needs machine code, and the file contains
7.96+ bits/byte of ciphertext. Loading it would produce an ARM listing of random
bytes.

To get code out of it, in order:

1. obtain a ROM dump of an R8 (or any dumped DIGIC X body for the shared key);
2. run `d8_oracle`-style emulation of the dumped crypto functions to derive the
   FIR decryption key;
3. decrypt, then Ghidra has real ARMv7 code.

Step 3 is the easy one. Steps 1 and 2 are exactly the problem this session has
been circling: no public dump, and no public key.
