#!/usr/bin/env python3
"""Index strings in the 6D ROM dumps and find code that references them.

    ./romscan.py find WLAN            # strings matching WLAN, with addresses
    ./romscan.py refs 0xf8caedf0      # 32-bit literals pointing at an address
    ./romscan.py near 0xf84324c4      # code locations referencing that region

The dumps are qemu/6D/ROM0.BIN (base 0xF0000000) and ROM1.BIN (0xF8000000).
"""
import sys, struct, os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ROMS = [("ROM0", os.path.join(ROOT, "qemu/6D/ROM0.BIN"), 0xF0000000),
        ("ROM1", os.path.join(ROOT, "qemu/6D/ROM1.BIN"), 0xF8000000)]


def strings(minlen=6):
    for name, path, base in ROMS:
        data = open(path, "rb").read()
        start = None
        for i, b in enumerate(data):
            if 32 <= b < 127:
                if start is None:
                    start = i
            else:
                if start is not None and i - start >= minlen:
                    yield name, base + start, data[start:i].decode("latin-1")
                start = None


def cmd_find(pat):
    pat = pat.lower()
    for name, addr, s in strings():
        if pat in s.lower():
            print(f"{name} 0x{addr:08x} {s}")


def refs(addr):
    pat = struct.pack("<I", addr & 0xFFFFFFFF)
    for name, path, base in ROMS:
        data = open(path, "rb").read()
        i = 0
        while True:
            i = data.find(pat, i)
            if i < 0:
                break
            print(f"{name} code@0x{base + i:08x} -> 0x{addr:08x}")
            i += 4


def cmd_near(addr, span=0x10000):
    for name, path, base in ROMS:
        data = open(path, "rb").read()
        for i in range(len(data) // 4):
            w = struct.unpack_from("<I", data, i * 4)[0]
            if abs(w - addr) < span:
                print(f"{name} code@0x{base + i * 4:08x} -> 0x{w:08x}")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    cmd, arg = sys.argv[1], sys.argv[2]
    if cmd == "find":
        cmd_find(arg)
    elif cmd == "refs":
        refs(int(arg, 16))
    elif cmd == "near":
        cmd_near(int(arg, 16))
    else:
        print(__doc__)
