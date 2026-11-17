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
