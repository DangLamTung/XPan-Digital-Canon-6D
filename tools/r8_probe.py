#!/usr/bin/env python3
"""Probe a DIGIC X Canon body with miniPtp (lclevy), read-only.

miniPtp is the library that was tested against a real EOS R6, so it is a better
base than our hand-rolled client for the R8 question: it can open a PTP/IP
session (with Canon's "Remote control (EOS Utility)" pairing) or USB, and it
exposes a generic transaction() so we can look at the operations that matter
without calling any of them.

    ./tools/r8_probe.py --ip 192.168.1.19     # WiFi, pairing as in miniPtp's readme
    ./tools/r8_probe.py --usb                 # USB (libusb; on Windows use Zadig)

It prints the model, the firmware, every vendor op, and flags the four that
matter for the bootflag question:

    0x9050 / 0x905c   Initiate event procedure (the CBasic-era way to call code)
    0x9060            IsNeoKabotanProcMode -- Canon's factory/service mode check
    0x911f            EOS_UpdateFirmware

Nothing is invoked except GetDeviceInfo / OpenSession / GetDevicePropDesc.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "pylibs"))
sys.path.insert(0, os.path.join(HERE, "third_party", "miniPtp"))

# miniPtp2 opens "ptp.yml" relative to the working directory
MINIPTP = os.path.join(HERE, "third_party", "miniPtp")
os.chdir(MINIPTP)
from miniPtp2 import ptp   # noqa: E402

FLAGS = {
    0x9050: "InitiateEventProc_50", 0x9051: "TerminateEventProc_51",
    0x905C: "InitiateEventProc_5c", 0x905D: "TerminateEventProc_5d",
    0x9060: "IsNeoKabotanProcMode", 0x911F: "EOS_UpdateFirmware",
    0x9033: "GetMacAddress", 0x9108: "EOS_GetDeviceInfoEx",
    0x9116: "EOS_GetEvent", 0x9109: "EOS_GetObjectInfoEx",
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip")
    ap.add_argument("--usb", action="store_true")
    a = ap.parse_args()

    obj = ptp(transport="ip", ip_addr=a.ip) if a.ip else ptp()
    info = obj.get_device_info()
    print("model    :", info.get("Model"))
    print("firmware :", info.get("Device_version"))
    ops = sorted(info["operations_supported"])
    print("operations:", len(ops))
    vendor = [o for o in ops if o >= 0x9000]
    print("vendor ops:", len(vendor))
    for o in vendor:
        print("   0x%04x  %s" % (o, FLAGS.get(o, "")))
    print()
    hit = [o for o in (0x9050, 0x905C, 0x9060, 0x911F) if o in ops]
    if hit:
        print("RELEVANT:", ", ".join("0x%04x/%s" % (o, FLAGS[o]) for o in hit))
    else:
        print("RELEVANT: none of 0x9050/0x905c/0x9060/0x911f are advertised")

    obj.open_session()
    props = sorted(info["device_prop_supported"])
    print("\nproperties:", len(props))
    for p in props[:40]:
        try:
            d = obj.get_device_prop_desc(p)
            cur = d.get("current") if isinstance(d, dict) else d
        except Exception as e:
            cur = "(%s)" % e
        print("   0x%04x  %s" % (p, cur))


if __name__ == "__main__":
    main()
