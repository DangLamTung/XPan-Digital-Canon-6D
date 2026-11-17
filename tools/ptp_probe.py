#!/usr/bin/env python3
"""Read-only PTP probe for Canon bodies, over USB or PTP/IP.

The point is to answer one question before anything dangerous is tried: does this
body advertise PTP operation 0x9052 (Canon's event-procedure call, the one that
enabled the bootflag on a 1300D)? The probe only ever sends GetDeviceInfo
(0x1001) and OpenSession (0x1002). It never invokes a vendor operation.

    ./tools/ptp_probe.py --scan                 # find a PTP/IP camera on the LAN
    ./tools/ptp_probe.py --ip 192.168.1.42      # probe it over WiFi
    ./tools/ptp_probe.py --usb                  # probe over USB (needs pyusb)

Output: model, firmware, every vendor operation the body lists, and a verdict on
0x9052.
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "pylibs"))   # vendored pyusb
import ptpip

# Canon event-procedure call. Petabyte, June 2021, 1300D bootflag over PTP.
EVENT_PROC = 0x9052

KNOWN = {
    0x1001: "GetDeviceInfo", 0x1002: "OpenSession", 0x1004: "GetStorageIDs",
    0x1007: "GetObjectHandles", 0x1008: "GetObjectInfo", 0x1009: "GetObject",
    0x100C: "SendObjectInfo", 0x100D: "SendObject", 0x100E: "DeleteObject",
    0x100F: "FormatStore", 0x1014: "GetDevicePropDesc",
    0x1015: "GetDevicePropValue", 0x1016: "SetDevicePropValue",
    0x9001: "GetEvent", 0x9003: "SetRemoteMode",
    0x9052: "CallEventProcedure  <-- the bootflag route",
    0x9101: "Canon EOS GetEvent", 0x9105: "Canon EOS SetRemoteMode",
    0x9114: "Canon EOS SetPCConnectMode", 0x9115: "Canon EOS SetEventMode",
    0x9116: "Canon EOS SetPropValue", 0x9117: "Canon EOS GetPropValue",
    0x9126: "Canon EOS KeepDeviceOn", 0x9128: "Canon EOS GetDeviceInfoEx",
}


def report(model, firmware, ops):
    print("model    :", model or "(none reported)")
    print("firmware :", firmware or "(none reported)")
    print("operations:", len(ops))
    vendor = [o for o in ops if o >= 0x9000]
    print("vendor ops:", len(vendor))
    for o in vendor:
        print("   0x%04x  %s" % (o, KNOWN.get(o, "")))
    print()
    if EVENT_PROC in ops:
        print("VERDICT: 0x9052 IS advertised -- the event-procedure call exists on "
              "this body. Do not call it blind; the parameter encoding matters "
              "(SRSA, 06/02/2022).")
    else:
        print("VERDICT: 0x9052 is NOT advertised -- this body does not offer the "
              "1300D bootflag route over PTP.")


def split_device_info(d):
    """Return (model, firmware, ops) from a GetDeviceInfo data phase."""
    o = 0
    o += 2 + 4 + 2
    n = d[o]; o += 1 + 2 * n
    o += 2
    cnt = struct.unpack_from("<I", d, o)[0]; o += 4
    ops = list(struct.unpack_from("<%dH" % cnt, d, o)); o += 2 * cnt

    def ptp_str():
        nonlocal o
        n = d[o]; o += 1
        s = d[o:o + 2 * n].decode("utf-16-le", "replace").rstrip("\x00")
        o += 2 * n
        return s
    try:
        manufacturer = ptp_str()
        model = ptp_str()
        firmware = ptp_str()
        serial = ptp_str()
    except Exception:
        manufacturer, model, firmware, serial = "", "", "", ""
    return (model or manufacturer), firmware, ops


def probe_ip(ip, port=15740):
    c = ptpip.Client(ip, port, timeout=10, verbose=True)
    c.connect()
    c.init()
    c.open_session()
    data = c.device_info()
    model, fw, ops = split_device_info(data)
    report(model, fw, ops)


def probe_usb():
    try:
        import usb.core
        import usb.util
    except ImportError:
        print("pyusb is not installed.  pip install pyusb")
        print("PTP over USB needs it; the PTP/IP path does not.")
        return 1
    dev = usb.core.find(idVendor=0x04A9)
    if dev is None:
        print("no Canon USB device found (VID 04A9)")
        return 1
    cfg = dev.get_active_configuration()
    intf = None
    for i in cfg:
        if i.bInterfaceClass == 6:      # still image
            intf = i
            break
    if intf is None:
        print("Canon device found but no still-image interface")
        return 1
    ep_in = usb.util.find_descriptor(intf, custom_match=lambda e: (
        usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN))
    ep_out = usb.util.find_descriptor(intf, custom_match=lambda e: (
        usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT))

    def send(code, params=()):
        body = struct.pack("<I", len(params))
        for p in params:
            body += struct.pack("<I", p)
        pkt = struct.pack("<IHHI", 12 + len(body), 1, code, 1) + body
        ep_out.write(pkt)

    def recv():
        head = bytes(ep_in.read(64, timeout=5000))[:4]
        if len(head) < 4:
            raise RuntimeError("short read")
        n = struct.unpack("<I", head)[0]
        buf = bytearray(head)
        while len(buf) < n:
            buf += bytes(ep_in.read(min(8192, n - len(buf)), timeout=5000))
        return buf

    send(0x1001)
    data = recv()
    if struct.unpack_from("<H", data, 6)[0] != 2:
        print("unexpected container type for GetDeviceInfo")
        return 1
    model, fw, ops = split_device_info(bytes(data[12:]))
    recv()      # response container
    report(model, fw, ops)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip")
    ap.add_argument("--port", type=int, default=15740)
    ap.add_argument("--scan", action="store_true")
    ap.add_argument("--usb", action="store_true")
    a = ap.parse_args()
    if a.usb:
        sys.exit(probe_usb())
    if a.scan:
        ips = ptpip.discover()
        if not ips:
            print("no PTP/IP camera found")
            sys.exit(1)
        for ip in ips:
            print("== %s" % ip)
            try:
                probe_ip(ip, a.port)
            except Exception as e:
                print("   failed:", e)
        return
    if a.ip:
        probe_ip(a.ip, a.port)
        return
    ap.print_help()


if __name__ == "__main__":
    main()
