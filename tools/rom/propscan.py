#!/usr/bin/env python3
"""Extract property descriptor tables from the 6D ROM1 image.

Entries are 16 bytes: id, flags, size, index. The AF family was found by
walking one such table backwards from a known entry; this scans the whole
image for runs of at least 4 consecutive plausible entries and prints them
grouped, so a property id can be tied to its payload size.
"""
import struct, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ROM = os.path.join(ROOT, "qemu/6D/ROM1.BIN")
BASE = 0xF8000000


def looks(pid, fl, sz, idx):
    return (0x80000000 <= pid <= 0x8005FFFF and fl <= 0x20
            and 0 < sz <= 0x1000 and idx < 0x10000)


def main():
    data = open(ROM, "rb").read()
    runs = []
    off, N = 0, len(data)
    while off < N - 16:
        pid, fl, sz, idx = struct.unpack_from("<IIII", data, off)
        if looks(pid, fl, sz, idx):
            run, o = [], off
            while o < N - 16:
                p2, f2, s2, i2 = struct.unpack_from("<IIII", data, o)
                if not looks(p2, f2, s2, i2):
                    break
                run.append((BASE + o, p2, s2))
                o += 16          # entries are 16 bytes apart, not 4
            if len(run) >= 4:
                runs.append(run)
            off = o
        else:
            off += 4
    print(f"{len(runs)} descriptor runs")
    for r in runs:
        print(f"\n# run at 0x{r[0][0]:08x}, {len(r)} entries")
        for a, pid, sz in r:
            print(f"  0x{pid:08x} size={sz:4d}  @0x{a:08x}")


if __name__ == "__main__":
    main()
