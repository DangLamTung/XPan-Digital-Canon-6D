#!/usr/bin/env python3
"""Bootflag over PTP on a DIGIC X body: the MLinstall recipe, run deliberately.

MLinstall (petabyt) enables Magic Lantern's autoexec path over USB with two event
procedures, and the command string it sends is literally "EnableBootDisk":

    0x9050  EOS_EnableEventProc      (up to three times)
    0x9052  EOS_ExecuteEventProc     data = packed ("EnableBootDisk")
    0x9053  EOS_GetEventProcReturnData

It refuses to try when the body advertises 0x9060 (IsNeoKabotanProcMode), with a
comment that calls this a "baseless assumption". That refusal is why DIGIC X looks
impossible in MLinstall -- not because the op was seen to fail on an R8.

    ./tools/r8_bootflag.py --ip 192.168.1.19            # check only, sends nothing
    ./tools/r8_bootflag.py --ip 192.168.1.19 --evproc "FA_GetProperty 0x2000005 0"
    ./tools/r8_bootflag.py --ip 192.168.1.19 --enable --yes

--check never sends anything. --enable and --evproc need --yes.
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "pylibs"))
MINIPTP = os.path.join(HERE, "third_party", "miniPtp")
sys.path.insert(0, MINIPTP)
os.chdir(MINIPTP)

from miniPtp2 import ptp   # noqa: E402

ENABLE_EVPROC = 0x9050
EXEC_EVPROC = 0x9052
EVPROC_RET = 0x9053
KABOTAN = 0x9060

TOK_INT, TOK_STR, TOK_TEXT = 2, 4, 0


def pack_evproc(text, args=(), long_strings=()):
    """Replicate canon_evproc_pack from MLinstall: name, count, params, footer."""
    out = bytearray(text.encode() + b"\x00")
    n = bytearray(struct.pack("<I", len(args)))
    for t in args:
        out += struct.pack("<IIIII", TOK_INT, t, 0, 0, 0)
    footer = bytearray(struct.pack("<I", len(long_strings)))
    for s in long_strings:
        out += struct.pack("<IIIII", TOK_STR, 0, 0, 0, len(s.encode()) + 1)
        footer += struct.pack("<I", 0) + s.encode() + b"\x00"
    struct.pack_into("<I", n, 0, len(args) + len(long_strings))
    out += n + footer
    return bytes(out)


def parse_request(text):
    """'FA_GetProperty 0x2000005 0' -> ('FA_GetProperty', [0x2000005, 0])."""
    parts = text.split()
    if not parts:
        raise SystemExit("empty event procedure")
    name = parts[0]
    args = [int(t, 0) for t in parts[1:]]
    return name, args


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip")
    ap.add_argument("--usb", action="store_true")
    ap.add_argument("--enable", action="store_true", help="send EnableBootDisk")
    ap.add_argument("--evproc", help="run one event procedure by name (read-only ones only)")
    ap.add_argument("--yes", action="store_true", help="required to send anything")
    a = ap.parse_args()

    obj = ptp(transport="ip", ip_addr=a.ip) if a.ip else ptp()
    info = obj.get_device_info()
    ops = set(info["operations_supported"])
    print("model    :", info.get("Model"))
    print("firmware :", info.get("Device_version"))
    for code, name in ((ENABLE_EVPROC, "EnableEventProc"), (EXEC_EVPROC, "ExecuteEventProc"),
                       (EVPROC_RET, "GetEventProcReturnData"), (KABOTAN, "IsNeoKabotanProcMode")):
        print("  0x%04x %-24s %s" % (code, name, "yes" if code in ops else "NO"))
    if KABOTAN in ops:
        print("\nnote: this body advertises 0x9060, so MLinstall refuses. The op list "
              "still shows whether 0x9050/0x9052 are there, which is the part MLinstall "
              "never checks.")

    if not (a.enable or a.evproc):
        return
    if not a.yes:
        print("\nrefusing to send: pass --yes")
        return
    if ENABLE_EVPROC not in ops or EXEC_EVPROC not in ops:
        print("\ncannot run: event procedures are not advertised")
        return

    obj.open_session()
    for i in range(3):
        r = obj.transaction(ENABLE_EVPROC, [], False)
        print("  EnableEventProc #%d -> 0x%04x" % (i + 1, r["ResponseCode"]))
        if r["ResponseCode"] == 0x2001:
            break

    if a.enable:
        name, args = "EnableBootDisk", []
    else:
        name, args = parse_request(a.evproc)
    data = pack_evproc(name, args)
    print("  ExecuteEventProc %r args=%s (%d bytes)" % (name, args, len(data)))
    r = obj.transaction(EXEC_EVPROC, [0, 0], True, senddata=data)
    print("  -> 0x%04x" % r["ResponseCode"])
    if r["ResponseCode"] == 0x2001 and EVPROC_RET in ops:
        r = obj.transaction(EVPROC_RET, [0, 0, 1], True)
        print("  return data:", r.get("Data"))


if __name__ == "__main__":
    main()
