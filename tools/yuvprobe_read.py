#!/usr/bin/env python3
"""Read ML/FILMSIM/YUV.BIN -- the shot-time frame probe written by film_sim.

The module patches two firmware entry points (0xFF1640E8 / 0xFF16E930, the
same ones Florin's Film Roll hooks) and, for one shot only, records what the
hooked function was handed plus two bands of candidate frame memory. This
script reads that dump and works out the layout of the frame Canon was about
to encode: the stride, which band is the luma plane, and whether the chroma is
planar or interleaved.

    ./yuvprobe_read.py YUV.BIN [--out DIR]

Nothing is assumed: the stride is found by autocorrelation (a frame buffer
shows a strong correlation at exactly one row stride), and the plane order is
decided by which band is smooth.
"""

import argparse
import os
import struct
import sys

from PIL import Image

# struct fs_yuv_dump in magiclantern/modules/film_sim/fs_yuvhook.c
# magic, pc, ncalls, nsamples, len, regs[6][6], row[4], src[4], full[14]
FMT = "<5I12I4I4I14I1I16I"   # fs_yuv_hdr as it is now: magic..len(5), regs[2][6]=12, row[4], src[4], full[14], ctx_at, ctx_words[16]
HDR = struct.calcsize(FMT)
MAGIC = 0x56595346   # "FSYV"


def parse(path):
    data = open(path, "rb").read()
    if len(data) < HDR:
        sys.exit(f"{path}: {len(data)} bytes, shorter than the {HDR}-byte header")
    f = struct.unpack(FMT, data[:HDR])
    hdr = {
        "magic": f[0], "pc": f[1], "calls": f[2], "nsamples": f[3], "len": f[4],
        "regs": [list(f[5 + 6 * i: 11 + 6 * i]) for i in range(2)],
        "row": list(f[17:21]),
        "src": list(f[21:25]),
        "full": list(f[25:39]),
        "ctx": f[39], "ctx_words": list(f[40:56]),
    }
    body = data[HDR:]
    bands = []
    for i in range(hdr["nsamples"]):
        band = body[i * hdr["len"]:(i + 1) * hdr["len"]]
        if band:
            bands.append(band)
    return hdr, bands


def stride_score(band, stride, limit=200000):
    """How well the band correlates with itself shifted by `stride` bytes."""
    n = min(len(band) - stride, limit)
    if n <= 0:
        return 0.0
    a = band[:n]
    b = band[stride:stride + n]
    # mean absolute difference, inverted: identical rows score 1.0
    diff = sum(abs(x - y) for x, y in zip(a, b)) / n
    return 1.0 - diff / 255.0


def find_stride(band):
    """Row stride, from the candidates a DIGIC 5 frame buffer can have.

    Widths in play are 5472/5760 (JPEG L) and 2736/2880 (half) for luma, so a
    byte stride is one of those, doubled if the chroma is interleaved.
    """
    cands = sorted({2736, 2880, 3648, 3840, 4096, 5472, 5760, 8192,
                    2736 * 2, 2880 * 2, 5472 * 2, 5760 * 2})
    scored = [(stride_score(band, s), s) for s in cands if s < len(band)]
    scored.sort(reverse=True)
    return scored


def preview(band, stride, path, height=64):
    rows = min(height, len(band) // stride)
    if rows <= 0 or stride > 8192:
        return None
    img = Image.frombytes("L", (stride, rows), band[:stride * rows])
    img.save(path)
    return img.size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    if not os.path.exists(args.dump):
        sys.exit(f"not found: {args.dump}")

    hdr, bands = parse(args.dump)
    out = args.out or os.path.dirname(os.path.abspath(args.dump))

    print(f"magic {'ok' if hdr['magic'] == MAGIC else hex(hdr['magic'])}"
          f"   hooked site {hdr['pc']:08x}   hook calls {hdr['calls']}")
    print(f"hook calls recorded: {hdr['calls']}   samples: {hdr['nsamples']}")
    for i, r in enumerate(hdr["regs"]):
        if not any(r):
            continue
        print(f"   call {i}: " + "  ".join(f"r{j}={v:08x}" for j, v in enumerate(r)))
    cw = hdr["ctx_words"]
    if hdr["ctx"]:
        print(f"   encoder context at {hdr['ctx']:08x}:")
        for i in range(0, 16, 4):
            print("     +%02x: " % (i * 4) + "  ".join(f"{w:08x}" for w in cw[i:i + 4]))
        for i, w in enumerate(cw):
            if w in (3648, 3840, 5472, 5760, 2736, 1824):
                print(f"     +{i*4:02x} = {w}  <-- a frame dimension")
    if hdr["nsamples"]:
        print("   sampled rows: " + "  ".join(f"row {r} -> {s:08x}"
              for r, s in zip(hdr["row"], hdr["src"]) if s))

    if not bands:
        sys.exit("no bands captured: the hook found no plausible frame pointer")

    for i, band in enumerate(bands):
        smooth = sum(abs(a - b) for a, b in zip(band[1:], band[:-1])) / max(1, len(band) - 1)
        print(f"\nband {i} (row {hdr['row'][i]}): {len(band)} bytes from {hdr['src'][i]:08x},"
              f" mean |d/dbyte| {smooth:.1f}"
              f" ({'smooth - luma-like' if smooth < 18 else 'busy - chroma or detail'})")
        scored = find_stride(band)
        for score, stride in scored[:4]:
            print(f"   stride {stride:5d}: self-correlation {score:.3f}")
        best = scored[0][1]
        path = os.path.join(out, f"YUV_band{i}_w{best}.png")
        size = preview(band, best, path)
        if size:
            print(f"   wrote {path} as {size[0]}x{size[1]} greyscale")

    print("\nNext: the band with the highest self-correlation is the luma plane;"
          "\nits stride is the row pitch and the plane size is width*height.")


if __name__ == "__main__":
    main()
