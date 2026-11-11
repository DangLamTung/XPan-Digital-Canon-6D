#!/usr/bin/env python3
"""Join generated canister records onto the user's FILMCAN.DAT.

FILMCAN.DAT covers fifteen emulsions with a shared 170-colour palette.
tools/make_can_art.py draws the eleven it does not cover, in that same palette.
This writes one file containing all twenty-six:

    ML/FILMSIM/CANS.DAT

The module reads that file first and falls back to ML/data/FILMCAN.DAT, so the
manufacturer artwork the user already has stays untouched and nothing of it is
copied into this repository.

    python3 tools/merge_cans.py --base "<card>/ML/data/FILMCAN.DAT" \\
        --art /tmp/cans --out "<card>/ML/FILMSIM/CANS.DAT"
"""
import argparse
import os
import struct
import sys

# Index of each generated record in the output, after the base file's records.
# The module's fs_can_map uses exactly this order.
ORDER = [
    "CineStill_50D", "Color_800", "C200", "Provia_100F", "TMax_100",
    "TMax_P3200", "Delta_100", "Pan_F_Plus_50", "Acros_100", "Fomapan_100",
    "XP2_Super",
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--art", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    raw = open(args.base, "rb").read()
    if raw[:4] != b"FCAN":
        sys.exit("%s is not a FILMCAN.DAT" % args.base)
    count, ncolors, w, h, tw, th = struct.unpack("<6H", raw[4:16])
    off = 16 + ncolors * 3
    img_sz, thm_sz = w * h, tw * th
    need = off + count * (img_sz + thm_sz)
    if len(raw) != need:
        sys.exit("base size %d, header says %d" % (len(raw), need))

    palette = raw[16:off]
    images = raw[off:off + count * img_sz]
    thumbs = raw[off + count * img_sz:]

    new_images, new_thumbs = [], []
    for name in ORDER:
        ip = os.path.join(args.art, name + ".IMG")
        tp = os.path.join(args.art, name + ".THM")
        if not (os.path.exists(ip) and os.path.exists(tp)):
            sys.exit("missing %s / %s - run tools/make_can_art.py first"
                     % (ip, tp))
        i = open(ip, "rb").read()
        t = open(tp, "rb").read()
        if len(i) != img_sz or len(t) != thm_sz:
            sys.exit("%s is %d/%d bytes, expected %d/%d"
                     % (name, len(i), len(t), img_sz, thm_sz))
        if max(i) >= ncolors and max(i) != 255:
            sys.exit("%s uses an index outside the palette" % name)
        new_images.append(i)
        new_thumbs.append(t)

    total = count + len(ORDER)
    out = bytearray()
    out += b"FCAN"
    out += struct.pack("<6H", total, ncolors, w, h, tw, th)
    out += palette
    out += images
    out += b"".join(new_images)
    out += thumbs
    out += b"".join(new_thumbs)

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(out)
    print("%s: %d records (%d base + %d drawn), %d colours, %d bytes"
          % (args.out, total, count, len(ORDER), ncolors, len(out)))


if __name__ == "__main__":
    sys.exit(main())
