#!/usr/bin/env python3
"""Inspect a Canon FIR and, if a body is connected, drive the PTP update op.

The update protocol itself is small and fully known (miniPtp's canon_update.py):

    op 0x911f (EOS_UpdateFirmware)
    params : [ total_length, offset ]
    data   : 32-byte file name, NUL padded, then a chunk of the FIR

repeat with offset += chunk until the whole file has been sent. There is no
verify-only op: sending the file *is* the update request, and the camera decides.

    ./tools/fir_push.py --inspect EOSR8140.FIR
    ./tools/fir_push.py --inspect EOSR8140.FIR --ip 192.168.1.19   # also compare ids
    ./tools/fir_push.py --push EOSR8140.FIR --ip 192.168.1.19 --yes

`--inspect` never talks to a camera. `--push` performs a real update and needs
--yes; with a Canon-signed file that is a genuine (re)flash or downgrade.
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "pylibs"))
MINIPTP = os.path.join(HERE, "third_party", "miniPtp")
sys.path.insert(0, MINIPTP)

UPDATE = 0x911F
CHUNK = 0x200000 - 32          # miniPtp's PTP_DATA_LEN minus the name field


def inspect(path):
    d = open(path, "rb").read()
    size = len(d)
    model, = struct.unpack_from("<I", d, 0)
    ver = d[0x10:0x1c].split(b"\x00")[0].decode("latin-1")
    fsize, = struct.unpack_from("<I", d, 0x38)
    blobs = []
    for o in (0x6c, 0x90, 0xb4, 0xd8):
        blobs.append(d[o:o + 0x20].hex())
    print("file     :", os.path.basename(path))
    print("size     : %d (0x%x)" % (size, size))
    print("model id : 0x%08x" % model)
    print("version  :", ver)
    print("size fld : 0x%x   %s" % (fsize, "matches file" if fsize == size else "MISMATCH"))
    print("sig blobs:", len(blobs), "x 32 bytes")
    for i, b in enumerate(blobs, 1):
        print("   %d: %s..." % (i, b[:16]))
    print("sections : 0x%08x 0x%08x 0x%08x"
          % (struct.unpack_from("<I", d, 0x5c)[0],
             struct.unpack_from("<I", d, 0x60)[0],
             struct.unpack_from("<I", d, 0x64)[0]))
    return model, ver, size, d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("fir")
    ap.add_argument("--inspect", action="store_true")
    ap.add_argument("--push", action="store_true")
    ap.add_argument("--ip")
    ap.add_argument("--usb", action="store_true")
    ap.add_argument("--yes", action="store_true")
    a = ap.parse_args()

    model, ver, size, data = inspect(a.fir)
    if not a.push:
        if a.ip or a.usb:
            os.chdir(MINIPTP)
            from miniPtp2 import ptp
            obj = ptp(transport="ip", ip_addr=a.ip) if a.ip else ptp()
            info = obj.get_device_info()
            print("\ncamera   :", info.get("Model"), info.get("Device_version"))
            print("0x911f   :", "yes" if UPDATE in info["operations_supported"] else "NO")
            mid = info.get("Model_id")
            if mid is not None:
                print("model id : 0x%08x  file 0x%08x  %s"
                      % (mid, model, "match" if mid == model else "MISMATCH"))
        return

    if not a.yes:
        print("\nrefusing to push: pass --yes (this starts a real update)")
        return
    os.chdir(MINIPTP)
    from miniPtp2 import ptp
    obj = ptp(transport="ip", ip_addr=a.ip) if a.ip else ptp()
    info = obj.get_device_info()
    if UPDATE not in info["operations_supported"]:
        print("camera does not advertise 0x911f"); return
    obj.open_session()
    name = (os.path.basename(a.fir).encode() + b"\x00" * 32)[:32]
    sent = 0
    while sent < len(data):
        r = obj.transaction(UPDATE, [len(data), sent],
                            senddata=name + data[sent:sent + CHUNK])
        print("  offset %9d -> 0x%04x" % (sent, r["ResponseCode"]))
        if r["ResponseCode"] != 0x2001:
            print("  stopped at 0x%04x" % r["ResponseCode"]); return
        sent += min(CHUNK, len(data) - sent)
    print("all %d bytes sent" % len(data))


if __name__ == "__main__":
    main()
