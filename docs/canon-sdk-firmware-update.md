# What the Canon EDSDK 13.21.10 actually does with firmware

Read from `EDSDK132110CDwithRAW(13.21.10).zip`, macOS framework
`EDSDK.framework/Versions/A/EDSDK`, arm64 slice. Everything below is quoted from
`otool -tvV` output, not inferred from the public API.

## The SDK

```
Release 08/03/2026 · adds EOS R8 Mark II · macOS 26 (drops macOS 13)
116 kEdsPropID_* · 56 Eds* functions · separate RAW-develop library (DPP.framework)
```

## Firmware is not in the public API

No `EdsUpdateFirmware` in `EDSDK.h`. The path exists only as C++ internals:

```
CPtpInterface::DS_UpdateFirmware(void*, char const*, SEdsEventHandler*)   @ 0x6c354
CPtpCamera::UpdateFirmware(int, void const*)                              @ 0x61fd0
```

and it logs through the SDK's own logger:

```
PTP   DS_UpdateFirmware (filePath=%s)
PTP   DS_UpdateFirmware (length=%d, fileSize=%d, progress=%d)
```

## The two opcodes it uses

`0x911f` — the upload, inside `DS_UpdateFirmware` at 0x6c504:

```
000000000006c504  mov  w8, #0x911f        ; opcode
000000000006c508  str  w8, [sp, #0x40]    ; command struct starts here
000000000006c50c  mov  w8, #0x2
000000000006c510  strh w8, [sp, #0x44]    ; parameter count = 2
000000000006c518  stp  w8, w25, [sp, #0x48]  ; the two parameters
000000000006c51c  ldr  x8, [x22, #0x10]
000000000006c520  str  x8, [sp, #0x60]    ; data-phase pointer
000000000006c538  blr  x8                 ; virtual send at vtable +0x130
000000000006c548  add  w8, w8, #0x20      ; progress accumulator
```

So the upload is a two-parameter vendor command `0x911f` with a data phase —
the same opcode `tools/fir_push.py` already sends.

`0x9107` — the read-back, inside `CPtpInterface::DS_GetPartialObject`:

```
0000000000064d00  mov  w8, #0x9107        ; vendor partial read
0000000000064d04  mov  w9, #0x101b        ; standard GetPartialObject
0000000000064d0c  csel w8, w9, w8, eq     ; w27 == 0 ? 0x911f path : standard
0000000000064d14  mov  w8, #0x3
0000000000064d18  strh w8, [sp, #0x24]    ; parameter count = 3
```

So Canon reads progress/status back with a three-parameter command, using the
standard `0x101b` when a flag is zero and vendor `0x9107` otherwise.

## What this does and does not settle

Settled:

* the upload opcode is `0x911f`, confirmed independently of our own tool;
* it takes two parameters and a data phase, and the SDK tracks a byte counter
  plus a file size — i.e. chunked sending, not one shot;
* status is read with a three-parameter command, `0x9107`/`0x101b`.

Not settled, and not answerable from this binary:

* whether the camera accepts an **older** version (the SDK simply streams the
  file; any version or signature check happens on the body);
* whether `0x911f` needs an unlock command first on DIGIC X bodies;
* what the two parameters mean (candidate: offset and chunk length, but the
  disassembly builds them from a counter and a file size, not from names).

## What would answer the open questions

A capture of `DS_UpdateFirmware` running once against a real body — USBPcap or
`tools/ptpcap.py` — would give the parameter values and the reply codes in order.
Without that, "downgrade by hand" stays a possibility, not a method.

## Does the SDK have any path to the boot flags? No

Enumerated every vendor opcode in the arm64 slice (174 constants, `mov w?, #0x9xxx`
with the enclosing symbol). There is **no `0x9050` and no `0x9052`** anywhere in the
binary:

```
$ grep -cE "0x9050|0x9052" edsdk.asm
0
```

and no string matching `boot`, `bootdisk`, `bootflag`, `UpdateMode` or
`firmwareupdate`. So the event-procedure pair that MLinstall uses to call
`EnableBootDisk` is **not exposed through this SDK**, in any version of its
public or internal API.

The only firmware-adjacent path is `0x911f` above, which streams a file into the
body's update routine. A command that flips the boot flags is a different
mechanism entirely, and Canon keeps it out of EDSDK.

## What else the opcode table gives us

Selected vendor opcodes, with the function that issues them:

| opcode | function | note |
|---|---|---|
| `0x9101`–`0x9107` | GetStorageIDs, GetStorageInfo, GetObjectInfo, GetObject, DeleteObject, FormatStore, GetPartialObject | the standard storage set, in Canon numbering |
| `0x9113` / `0x9114` | Get/SetRemoteMode | what the SDK calls before remote capture |
| `0x9115` / `0x9116` | SetEventMode / GetEvent | property event subscription |
| `0x911f` | UpdateFirmware | streams a `.FIR` |
| `0x9193` | `DS_WriteLookFile` | **writes a look file to the body** — the closest thing in the SDK to pushing a recipe |
| `0x9199` | SetCsdFileData | the CSD file introduced in 13.20.11 |
| `0x9205` / `0x9206` | SetPrivateKeyData / DeletePrivateKeyData | key material, not exposed publicly |
| `0x918a` / `0x9192` | SetUserPictureStyleData / …PCFlavorParam | picture-style flavour parameters |

`0x9193 DS_WriteLookFile` is worth a look for this project: it is a vendor command
that hands the body a look file, which is exactly the shape of our `.fs` push
(`0x9998`). The two are not the same opcode, but comparing their parameters would
say whether Canon already has a channel we could piggy-back on instead of the
module's own.
