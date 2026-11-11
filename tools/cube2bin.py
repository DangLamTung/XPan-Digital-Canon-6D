#!/usr/bin/env python3
"""Convert an Adobe/Resolve .cube into the packed form the camera reads.

The camera cannot hold a .cube: the text is 786 KB at 32^3 and the develop's
buffers already use most of what Magic Lantern can spare. So the cube is
resampled here, on the Mac, to 17^3 and written as RGB16:

    "FLUT"  u16 size  u16 reserved  size^3 * 3 little-endian u16

Red index varies fastest, which is .cube's own order and the order
se_lut3_apply_row() expects.

    python3 tools/cube2bin.py in.cube out.bin [--size 17]

Resampling is trilinear over the source cube. The source is already a sampled
function, so sampling it again at fewer points loses nothing a smooth colour
transform can show; 17^3 is what Resolve calls a "Medium" cube and what most
cameras ship.
"""
import argparse
import struct
import sys


def parse_cube(path):
    size = None
    dmin = (0.0, 0.0, 0.0)
    dmax = (1.0, 1.0, 1.0)
    vals = []
    with open(path, "r", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            up = line.upper()
            if up.startswith("LUT_3D_SIZE"):
                size = int(line.split()[1])
                continue
            if up.startswith("LUT_1D_SIZE"):
                raise SystemExit("1D .cube files are not a colour grade: " + path)
            if up.startswith("DOMAIN_MIN"):
                dmin = tuple(float(v) for v in line.split()[1:4])
                continue
            if up.startswith("DOMAIN_MAX"):
                dmax = tuple(float(v) for v in line.split()[1:4])
                continue
            if up[0].isalpha():
                continue          # TITLE and other keywords
            parts = line.split()
            if len(parts) < 3:
                continue
            vals.append((float(parts[0]), float(parts[1]), float(parts[2])))
    if not size:
        raise SystemExit("no LUT_3D_SIZE in " + path)
    if len(vals) != size ** 3:
        raise SystemExit(f"{path}: {len(vals)} entries, expected {size ** 3}")
    return size, dmin, dmax, vals


def sample(size, dmin, dmax, vals, r, g, b):
    """Trilinear value of the source cube at normalised (r,g,b) in 0..1."""
    def axis(t, lo, hi):
        # position in the source grid
        x = (lo + t * (hi - lo)) * (size - 1)
        x = 0.0 if x < 0.0 else (size - 1.0 if x > size - 1.0 else x)
        i = int(x)
        i = min(i, size - 2) if size > 1 else 0
        return i, x - i

    ir, fr = axis(r, dmin[0], dmax[0])
    ig, fg = axis(g, dmin[1], dmax[1])
    ib, fb = axis(b, dmin[2], dmax[2])

    out = [0.0, 0.0, 0.0]
    for c in range(3):
        def at(dr, dg, db):
            idx = (ib + db) * size * size + (ig + dg) * size + (ir + dr)
            return vals[idx][c]
        c00 = at(0, 0, 0) * (1 - fr) + at(1, 0, 0) * fr
        c01 = at(0, 1, 0) * (1 - fr) + at(1, 1, 0) * fr
        c10 = at(0, 0, 1) * (1 - fr) + at(1, 0, 1) * fr
        c11 = at(0, 1, 1) * (1 - fr) + at(1, 1, 1) * fr
        c0 = c00 * (1 - fg) + c01 * fg
        c1 = c10 * (1 - fg) + c11 * fg
        out[c] = c0 * (1 - fb) + c1 * fb
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cube")
    ap.add_argument("bin")
    ap.add_argument("--size", type=int, default=17,
                    help="target cube size (17 = 29 KB, 33 = 215 KB)")
    a = ap.parse_args()

    size, dmin, dmax, vals = parse_cube(a.cube)
    n = a.size
    if n < 2 or n > 65:
        raise SystemExit("size out of range")

    body = bytearray()
    for b in range(n):
        for g in range(n):
            for r in range(n):
                v = sample(size, dmin, dmax, vals,
                           r / (n - 1.0), g / (n - 1.0), b / (n - 1.0))
                for c in range(3):
                    x = int(round(min(1.0, max(0.0, v[c])) * 65535.0))
                    body += struct.pack("<H", x)

    with open(a.bin, "wb") as f:
        f.write(b"FLUT" + struct.pack("<HH", n, 0) + body)
    print(f"{a.cube} ({size}^3) -> {a.bin} ({n}^3, {8 + len(body)} bytes)")


if __name__ == "__main__":
    main()
