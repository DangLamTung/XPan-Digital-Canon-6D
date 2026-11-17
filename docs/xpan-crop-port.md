# Cropping inside Canon's JPEG encoder: what XPan does, and what the 6D has

Source: anibarro/magiclantern_simplified, tag v1.1, release "XPan v1.1 for the
Canon EOS M (firmware 2.0.2)". Files read: `README-XPAN.md`,
`src/xpan-native-jpeg.c`.

## What XPan actually does

It does **not** re-encode anything. It hooks two functions in Canon's JPEG path
and swaps the setup descriptor for a private copy:

```
#define JPEG_BEFORE 0xff4288b0u      /* Canon's JPEG setup */
#define JPEG_AFTER  0xff428f44u      /* after the encode job is queued */
```

At `JPEG_BEFORE`, when `regs[0] == 0x70298` (the job descriptor pointer it
watches for), it:

1. copies the 44-word setup descriptor into its own workspace;
2. replaces `words[2]`, the **profile pointer**, with a private copy of Canon's
   register/value profile (`0xff9125a8`, 96 pairs, or `0xff912bd0`, 101 pairs);
3. rewrites the four pipeline height registers inside that profile, e.g.
   `0xc0f1134c = 0x01af0008` -> the high half `0x1af` is `431`, i.e.
   `(3456/8)-1` for the EOS M's 3456 rows; the low half `0x0008` is the
   8-row unit;
4. sets `words[0x3c/4] = height` and `words[0x5c/4] = top * pitch` (EOS M pitch
   10368), and redirects `regs[0]` to the private descriptor.

Canon's encoder then writes a JPEG of that height, with RAW keeping the full
frame. Every value is hash-checked against the EOS M ROM, and the descriptor is
validated inside RAM before use; a different firmware leaves Canon untouched.

## The 6D has the same structure

Searching ROM1 for the same pipeline registers finds a register/value table at
**`0xF8D4F1A0` .. `0xF8D4F4C0`, 100 pairs**, and the four height registers read:

```
0xc0f1134c = 0x01c70008      0x1c7 = 455  ->  (455+1) * 8 = 3648 rows
0xc0f1138c = 0x01c70008
0xc0f1140c = 0x01c70008
0xc0f1158c = 0x01c70008
```

That is exactly the EOS M pattern with the 6D's own geometry: `0x1af` (3456 rows)
becomes `0x1c7` (3648 rows), same low half. The EOS M's fifth register
`0xc0f2650c` is absent on the 6D.

## The numbers a 6D crop would need

Full width 5472, 3648 rows, pitch **10944** (from this module's own shot logs).

| for 65:24 | value |
|---|---|
| JPEG height | 2020 rows (same as Develop writes today) |
| top row | 814 |
| padded height `(h+15)&~15` | 2032 |
| height register value | `0x00fd0008` |
| descriptor byte offset | `814 * 10944` = `0x87EE80` |

`814..2834` is the span this module already measures at capture, so the crop
geometry agrees with what the module burns as bars today.

## What is still missing on the 6D

1. the address of Canon's JPEG setup routine (EOS M: `0xff4288b0`) -- XPan finds
   it by watching for the descriptor pointer in `r0`;
2. the descriptor pointer itself (EOS M: `0x70298`) -- a RAM address, not in the
   ROM image;
3. the profile's start and length on the 6D, and its hash, so the patch can
   refuse to run on any other build.

None of these can be read out of ROM alone: 1 and 2 need a live observation on
the body, which is what `modules/dev_tools/mpu_dump` and a hook logger are for.

## Risk, stated by the authors

XPan's own readme: "Disable all other Magic Lantern modules ... XPan changes
Canon's colour tables, JPEG encoder, sensor crop, key routing and several
properties." For the 6D this would be the same class of change: patching the
descriptor of Canon's encoder on every capture.

## Found on the 6D: the descriptor, the profile, and who consumes it

Searching the RAM dump (`film_eval/rom/RAM4.BIN`) for the row count `0x01c70008`
is what broke it open. That value appears inside a **237-pair register table in
RAM at `0x07D348`**, which is the runtime copy of the ROM table at `0xF8D4EE30`
(same 237 pairs). Exactly one word in RAM points at it:

```
0x0009F9D8 + 8  ->  0x0007D348        /* descriptor[2] = profile pointer */
```

so the descriptor is at **`0x0009F9D8`**, and it has the same shape XPan patches:

```
+0x00  0x0007D250      +0x08  0x0007D348   <- profile
+0x10  0x0007DAB8      +0x14  0x0007DB70
+0x18  0x0007DBF8      +0x38  0x0000FFFF
+0x3c  0x00000000      <- height, written per job
+0x50  0x60000000      +0x5c  0x00093834   <- byte offset, written per job
```

Three sibling descriptors sit next to it at `0x9F930`, `0x9FA80` and `0x9FB00`
(160-168 bytes apart), listed by the pointer table at `0xF816B348`.

### Who runs it

The descriptor is referenced from three ROM sites, all in the JPEG path where the
`sasEncodeJpeg` strings live:

```
0xF816B36C   0xF816CD4C   0xF816E308
```

Two functions take the descriptor in `r0` and hand it, with its profile, to a
common consumer:

```
f816b3f8  ldr  r0, [r4, #8]      @ profile
f816b3fc  mov  r3, r4            @ descriptor
f816b400  mov  r2, #3            @ mode
f816b408  bl   0xF9039874        @ configure the pipeline from (profile, desc)

f816b448  ldr  r0, [r4, #8]
f816b450  mov  r2, #5
f816b458  bl   0xF9039874
```

`0xF9039874` is the routine that walks the register/value pairs. That is the
equivalent of what XPan lets Canon do after it has redirected the descriptor.

### The registers to change for a crop

Inside the RAM profile, **nine** registers carry the full-frame row count
`0x01c70008` (0x1c7 = 455, i.e. 456 x 8 = 3648 rows):

```
0xc0f08590  0xc0f0d010  0xc0f2e114  0xc0f1134c  0xc0f1138c
0xc0f1158c  0xc0f1140c  0xc0f04a0c  0xc0f2640c
```

For 65:24 (2020 rows, padded to 2032, `(2032/8)-1 = 253 = 0xFD`) each becomes
`0x00FD0008`, and the descriptor's `+0x5c` offset becomes `814 * 10944 =
0x87EE80`. That is the whole edit: a 237-pair copy, nine values changed, one
descriptor field, and the descriptor's profile pointer redirected to the copy.

### What is still needed before writing code

1. **which capture path** the three descriptors belong to (main JPEG, thumbnail,
   LiveView?) -- patching the wrong one gives a cropped thumbnail or nothing;
2. confirmation that `0xF9039874` reads `[desc+0x3c]`/`[desc+0x5c]` (XPan's
   values assume it; on the 6D those fields were zero and `0x93834` in the
   snapshot, so they are written by whoever queues the job);
3. a live run to see the descriptor *during* a capture, which is exactly what
   `mlwifi`-style logging does for properties: dump 176 bytes at `0x9F9D8`
   before and after a shot.

None of this is patched yet, and nothing here has run on the camera.

## Decoding the descriptor family without a camera dump

No new dump was taken. The decode below uses three things already in the
repository: the ROM image, the RAM image saved earlier, and the EOS M source.

### Which pointer is which

Each of the four addresses in the table at `0xF816B358` turned out to be either a
descriptor or a data table, and the string that follows each reference names it:

| address | what it is | evidence |
|---|---|---|
| `0x0009F930` | **RAW path** descriptor | referenced next to `Size Over(RAW::%#lx)(Suite::%#lx)` |
| `0x0009F9D8` | **JPG path** descriptor -- the one a crop must touch | referenced next to `Size Over(JPG::%#lx)(Suite::%lx)`, and it is the one whose `+8` points at the 237-pair profile |
| `0x0009FA80` | data table (bytes `03 02 02 03 03 03 02 02 ...`) | the values are a quantisation/zig-zag matrix, not pointers |
| `0x0009FB00` | data table (bytes `01 05 01 00 01 01 01 01 ...`) | same |

So of the four, only two are descriptors, and the two differ by path: **A is RAW,
B is JPG**. The pattern is confirmed by the size-check messages: the RAW check is
logged beside `0x9F930`, the JPG check beside `0x9F9D8`.

### The layout, decoded by cross-body evidence

The field offsets are not guesswork: `xpan-native-jpeg.c` documents the same
Canon structure on the EOS M -- profile at `+8`, height at `+0x3c`, byte offset
at `+0x5c` -- and the 6D descriptor matches that structure wherever we can
observe it without a dump:

```
0x0009F9D8 + 0x08 = 0x0007D348   the 237-pair profile (the only pointer to it)
0x0009F9D8 + 0x3c = 0            a field that is zero except while a job runs
0x0009F9D8 + 0x5c = 0x00093834   a byte-offset-looking value in the snapshot
0x0009F9D8 + 0x50 = 0x60000000   a flag word, same as XPan's descriptor uses
```

### The profile, decoded from its own values

Inside `0x7D348`, nine registers carry the 8-row-unit frame height
`0x01c70008` (`0x1c7 = 455`, i.e. `456 x 8 = 3648`), and one carries the same
height in pixels:

```
0xc0f13010 = 0x00000e40        (3648 rows, plain pixels)
```

That last one is the *source* frame height: a crop changes the nine 8-row-unit
registers and the descriptor, not this one, because the encoder still reads the
full sensor frame.

### What the image does not contain

The routine both paths call -- `0xF9039874` with mode 3 from the main job,
`0xF903978C` with mode 2 from the size check -- is at **file offset `0x1039874`**,
about **1 MB past the end of the 16 MB ROM dump**. It is the same boundary that
stops the socket analysis. So the exact field semantics inside the consumer
cannot be read out of this image, and the decode above stops one layer above it:
we know what the descriptor *is* and which fields a crop needs, not how the
consumer uses them.

Two ways past that, neither of which needs a camera dump:

1. **Cross-body**: the EOS M's `0x4288b0` path performs the same job with the same
   descriptor shape, and `xpan-native-jpeg.c` documents what it does with `+0x3c`
   and `+0x5c`; assuming the 6D's consumer behaves the same is an assumption, but
   a documented one;
2. **A 17 MB image** of `0xF8000000..0xF9100000` -- not a RAM dump, just a ROM
   read one megabyte longer than what `platform/6D.116/consts.h` asks for
   (`ROM1_SIZE 0x01000000`).

Nothing has been patched, and none of this has run on the camera.
