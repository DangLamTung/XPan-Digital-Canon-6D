#!/usr/bin/env python3
"""Decode Canon PTP traffic from a USB capture (pcap with USBPcap, or usbmon text).

Tornado EOS, Canon's service tools and our own probe all speak PTP over USB.
There is no public description of the service-mode operations, so the practical
way to learn them is to capture the traffic and decode it here.

    # Windows: USBPcap + Wireshark, save as .pcap
    ./tools/ptpcap.py capture.pcap

    # Linux: modprobe usbmon; tcpdump -i usbmon1 -w cap.pcap
    ./tools/ptpcap.py cap.pcap

Prints every PTP container: direction, type, op code, parameters, data length.
"""
import argparse
import struct
import sys

OP = {
    0x1001: "GetDeviceInfo", 0x1002: "OpenSession", 0x1004: "GetStorageIDs",
    0x1007: "GetObjectHandles", 0x1008: "GetObjectInfo", 0x1009: "GetObject",
    0x100C: "SendObjectInfo", 0x100D: "SendObject", 0x100E: "DeleteObject",
    0x1014: "GetDevicePropDesc", 0x1015: "GetDevicePropValue",
    0x1016: "SetDevicePropValue", 0x9001: "GetEvent", 0x9003: "SetRemoteMode",
    0x9052: "CallEventProcedure", 0x9101: "CanonGetEvent",
    0x9114: "SetPCConnectMode", 0x9115: "SetEventMode",
    0x9116: "SetPropValue", 0x9117: "GetPropValue", 0x9126: "KeepDeviceOn",
    0x9128: "GetDeviceInfoEx",
}
RESP = {0x2001: "OK", 0x2002: "GeneralError", 0x2003: "SessionNotOpen",
        0x2004: "InvalidTransactionID", 0x2005: "OperationNotSupported",
        0x2006: "ParameterNotSupported", 0x2007: "IncompleteTransfer",
        0x2009: "InvalidStorageID", 0x200A: "InvalidObjectHandle",
        0x2019: "DevicePropNotSupported", 0x201A: "InvalidDevicePropFormat",
        0x201B: "InvalidDevicePropValue", 0x201C: "InvalidParameter",
        0x201D: "SessionAlreadyOpen", 0x201E: "TransactionCancelled",
        0x201F: "DestinationUnsupported", 0xA001: "VendorError1"}


def containers(buf, out):
    """Split a reassembled byte stream into PTP containers."""
    o = 0
    while o + 8 <= len(buf):
        length, ctype = struct.unpack_from("<IH", buf, o)
        if length < 8 or o + length > len(buf):
            break
        code, = struct.unpack_from("<H", buf, o + 6)
        txid = None
        params = []
        if ctype in (1, 3):
            if o + 12 <= len(buf):
                txid, = struct.unpack_from("<I", buf, o + 8)
            n = (length - 12) // 4
            params = list(struct.unpack_from("<%dI" % max(0, n), buf, o + 12)) if n > 0 else []
        out.append((ctype, code, txid, params, length))
        o += length


def read_pcap(path):
    d = open(path, "rb").read()
    magic = d[:4]
    if magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1"):
        endian, nano = "<", magic[0] == 0x4D
    elif magic in (b"\xa1\xb2\xc3\xd4", b"\xa1\xb2\x3c\x4d"):
        endian, nano = ">", magic[0] == 0xA1
    else:
        raise SystemExit("not a pcap file (pcapng is not supported; export as pcap)")
    linktype, = struct.unpack_from(endian + "I", d, 20)
    o = 24
    pkts = []
    while o + 16 <= len(d):
        ts, tsu, incl, orig = struct.unpack_from(endian + "IIII", d, o)
        o += 16
        pkts.append((linktype, d[o:o + incl], ts + (tsu / 1e9 if nano else tsu / 1e6)))
        o += incl
    return pkts


def usbpcap_stream(pkts):
    """Return {bus, device, endpoint, direction: bytearray} from a USBPcap capture."""
    streams = {}
    for linktype, data, ts in pkts:
        if linktype != 249 or len(data) < 27:
            continue
        hlen, = struct.unpack_from("<H", data, 0)
        if hlen < 27 or hlen > len(data):
            continue
        bus, dev, ep = struct.unpack_from("<HHB", data, 17)
        xfer, dlen = struct.unpack_from("<BI", data, 22)
        payload = data[hlen:]
        if not payload:
            continue
        direction = "in" if ep & 0x80 else "out"
        key = (bus, dev, ep & 0x7F, direction, xfer)
        st = streams.setdefault(key, bytearray())
        st += payload
    return streams


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--raw", action="store_true", help="dump data phases as hex")
    a = ap.parse_args()
    pkts = read_pcap(a.capture)
    streams = usbpcap_stream(pkts)
    if not streams:
        print("no USBPcap payloads found (linktype != 249?)")
        return
    for key in sorted(streams):
        bus, dev, ep, direction, xfer = key
        buf = bytes(streams[key])
        out = []
        containers(buf, out)
        if not out:
            continue
        print("=== bus %d dev %d ep 0x%02x %s xfer %d : %d containers"
              % (bus, dev, ep | (0x80 if direction == "in" else 0), direction, xfer, len(out)))
        for ctype, code, txid, params, length in out:
            kind = {1: "CMD", 2: "DATA", 3: "RESP"}.get(ctype, str(ctype))
            name = OP.get(code, RESP.get(code, "")) if kind != "DATA" else ""
            p = " ".join("%08x" % x for x in params[:4])
            print("  %-4s %-4s 0x%04x %-22s tid=%s len=%d %s"
                  % (kind, {1: ">", 2: "|", 3: "<"}.get(ctype, "?"), code,
                     name[:22], txid, length, p))
            if a.raw and kind == "DATA":
                print("       data:", buf[:64].hex())


if __name__ == "__main__":
    main()
