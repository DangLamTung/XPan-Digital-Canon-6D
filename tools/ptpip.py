#!/usr/bin/env python3
"""A minimal PTP/IP client, so the camera can be tested without gphoto2.

The 6D is a PTP/IP server on port 15740 (mDNS `_ptp._tcp`, TXT
`tid.canon.com=<GUID>`). This speaks the protocol directly: init handshake,
OpenSession, GetDeviceInfo -- which lists the operations the body accepts,
including SendObjectInfo (0x100C) and SendObject (0x100D) -- and optionally an
upload.

Packet layout follows CIPA DC-005. Nothing here is verified against a real body:
that is what the tool is for. Every failure prints the raw response so the next
step is data, not a guess.

    ./tools/ptpip.py --ip 192.168.1.2 probe
    ./tools/ptpip.py --ip 192.168.1.2 upload recipe.xml [NAME.XML]
"""
import argparse, os, socket, struct, sys, uuid

# packet types
INIT_CMD_REQ, INIT_CMD_ACK, INIT_EVT_REQ, INIT_EVT_ACK, INIT_FAIL = 1, 2, 3, 4, 5
OP_REQ, OP_RESP, EVENT, START_DATA, DATA, CANCEL, END_DATA = 6, 7, 8, 9, 10, 11, 12

OPS = {
    0x1001: "GetDeviceInfo", 0x1002: "OpenSession", 0x1003: "CloseSession",
    0x1004: "GetStorageIDs", 0x1005: "GetStorageInfo", 0x1006: "GetNumObjects",
    0x1007: "GetObjectHandles", 0x1008: "GetObjectInfo", 0x1009: "GetObject",
    0x100A: "GetThumb", 0x100B: "DeleteObject", 0x100C: "SendObjectInfo",
    0x100D: "SendObject", 0x100E: "InitiateCapture", 0x100F: "FormatStore",
    0x1014: "GetDevicePropDesc", 0x1015: "GetDevicePropValue",
    0x1016: "SetDevicePropValue", 0x101B: "GetPartialObject",
    0x9104: "Canon:9104", 0x9105: "Canon:9105", 0x9106: "Canon:9106",
    0x9107: "Canon:9107",
}
RESP = {0x2001: "OK", 0x2002: "GeneralError", 0x2003: "SessionNotOpen",
        0x2004: "InvalidTransactionID", 0x2005: "OperationNotSupported",
        0x2006: "ParameterNotSupported", 0x2007: "IncompleteTransfer",
        0x2009: "InvalidStorageID", 0x200A: "InvalidObjectHandle",
        0x200C: "StoreNotAvailable", 0x2019: "DevicePropNotSupported",
        0x201D: "InvalidParentObject", 0x2020: "DeviceBusy"}


def ptp_string(s):
    b = s.encode("utf-16-le") + b"\x00\x00"
    return bytes([len(b) // 2]) + b


class Client:
    def __init__(self, ip, port=15740, timeout=8, verbose=True):
        self.ip, self.port, self.timeout, self.verbose = ip, port, timeout, verbose
        self.sock = None
        self.txid = 0

    def log(self, *a):
        if self.verbose:
            print(*a)

    # ---- transport ------------------------------------------------------
    def connect(self):
        self.sock = socket.create_connection((self.ip, self.port), self.timeout)
        self.sock.settimeout(self.timeout)
        self.log("tcp connected to %s:%d" % (self.ip, self.port))

    def send(self, pkt):
        self.sock.sendall(pkt)

    def recv(self):
        head = b""
        while len(head) < 8:
            chunk = self.sock.recv(8 - len(head))
            if not chunk:
                raise ConnectionError("camera closed the connection")
            head += chunk
        length, ptype = struct.unpack("<II", head)
        body = b""
        while len(body) < length - 8:
            chunk = self.sock.recv(length - 8 - len(body))
            if not chunk:
                raise ConnectionError("short packet")
            body += chunk
        return ptype, body

    @staticmethod
    def packet(ptype, body=b""):
        return struct.pack("<II", 8 + len(body), ptype) + body

    # ---- handshake ------------------------------------------------------
    def init(self, guid=None, name="mlwifi-test"):
        guid = guid or uuid.uuid4().bytes
        body = guid + ptp_string(name) + struct.pack("<I", 0x00010000)
        self.send(self.packet(INIT_CMD_REQ, body))
        ptype, body = self.recv()
        if ptype == INIT_FAIL:
            reason = struct.unpack("<I", body[:4])[0]
            raise RuntimeError("InitFail reason=%d (5 usually means the camera "
                               "is paired with another device: reset its Wi-Fi "
                               "connection settings)" % reason)
        if ptype != INIT_CMD_ACK:
            raise RuntimeError("expected InitCommandAck, got type %d" % ptype)
        self.conn_number = struct.unpack("<I", body[:4])[0]
        self.log("init ok, connection %d" % self.conn_number)
        # The event channel is a *second* TCP connection to the same port.
        # It is optional for our purposes: we only poll, we never wait on events,
        # so a failure here is logged and ignored.
        try:
            ev = socket.create_connection((self.ip, self.port), self.timeout)
            ev.settimeout(self.timeout)
            ev.sendall(self.packet(INIT_EVT_REQ, struct.pack("<I", self.conn_number)))
            head = ev.recv(8)
            if len(head) == 8:
                _len, ptype = struct.unpack("<II", head)
                self.log("event channel: type %d" % ptype)
            self.ev_sock = ev
        except Exception as e:
            self.log("event channel unavailable (%s); continuing" % e)

    # ---- operations -----------------------------------------------------
    def op(self, code, params=(), data=b"", dataphase=None):
        """Send an operation; return (response_code, params, data_from_camera).

        dataphase 0 = none, 1 = data from camera to host, 2 = data from host to
        camera. When omitted it is chosen from whether `data` was given.
        """
        self.txid += 1
        tid = self.txid
        if dataphase is None:
            dataphase = 2 if data else 0
        body = struct.pack("<IHI", dataphase, code, tid)
        for p in params:
            body += struct.pack("<I", p)
        self.send(self.packet(OP_REQ, body))

        if data:
            self.send(self.packet(START_DATA, struct.pack("<IQ", tid, len(data))))
            self.send(self.packet(DATA, struct.pack("<I", tid) + data))
            self.send(self.packet(END_DATA, struct.pack("<I", tid)))

        rbuf = b""
        while True:
            ptype, body = self.recv()
            if ptype == EVENT:
                continue
            if ptype == START_DATA:
                rbuf = b""
                continue
            if ptype == DATA:
                rbuf += body[4:]
                continue
            if ptype == END_DATA:
                continue
            if ptype == OP_RESP:
                rcode = struct.unpack("<H", body[:2])[0]
                rtid = struct.unpack("<I", body[2:6])[0]
                extra = body[6:]
                rparams = [struct.unpack_from("<I", extra, i)[0]
                           for i in range(0, len(extra) - 3, 4)]
                if rtid != tid:
                    self.log("warning: response for txid %d, expected %d" % (rtid, tid))
                return rcode, rparams, rbuf
            self.log("unexpected packet type %d during op 0x%04x" % (ptype, code))

    def storage_ids(self):
        rc, _, d = self.op(0x1004, dataphase=1)
        if rc != 0x2001:
            raise RuntimeError("GetStorageIDs -> %s" % RESP.get(rc, rc))
        n = struct.unpack_from("<I", d, 0)[0]
        return list(struct.unpack_from("<%dI" % n, d, 4))

    def object_handles(self, storage, parent=0xFFFFFFFF, fmt=0):
        rc, _, d = self.op(0x1007, [storage, fmt, parent], dataphase=1)
        if rc not in (0x2001,):
            raise RuntimeError("GetObjectHandles -> %s" % RESP.get(rc, rc))
        n = struct.unpack_from("<I", d, 0)[0]
        return list(struct.unpack_from("<%dI" % n, d, 4))

    def object_info_of(self, handle):
        rc, _, d = self.op(0x1008, [handle], dataphase=1)
        if rc != 0x2001:
            return None
        o = 0
        storage = struct.unpack_from("<I", d, 0)[0]; o = 4
        fmt = struct.unpack_from("<H", d, o)[0]; o += 2
        o += 2                                  # ProtectionStatus
        size = struct.unpack_from("<I", d, o)[0]; o += 4
        o += 2 + 4 + 4 + 4 + 4 + 4 + 4          # thumbs, dimensions, depth
        o += 4 + 2 + 4 + 4                      # Parent, AssocType, AssocDesc, Seq
        n = d[o]; name = d[o + 1:o + 1 + 2 * (n - 1)].decode("utf-16-le", "replace") if n else ""
        o += 1 + 2 * n
        n = d[o]; date = d[o + 1:o + 1 + 2 * (n - 1)].decode("utf-16-le", "replace") if n else ""
        return {"handle": handle, "storage": storage, "format": fmt,
                "size": size, "name": name, "date": date}

    def thumb(self, handle):
        rc, _, d = self.op(0x100A, [handle], dataphase=1)
        return d if rc == 0x2001 else None

    def object_bytes(self, handle):
        rc, _, d = self.op(0x1009, [handle], dataphase=1)
        if rc != 0x2001:
            raise RuntimeError("GetObject -> %s" % RESP.get(rc, rc))
        return d

    def capture(self):
        rc, _, _ = self.op(0x100E, [0, 0], dataphase=0)
        return rc

    def delete(self, handle):
        rc, _, _ = self.op(0x100B, [handle], dataphase=0)
        return rc


    def open_session(self, sid=1):
        rc, extra, _ = self.op(0x1002, [sid], dataphase=0)
        if rc != 0x2001:
            raise RuntimeError("OpenSession -> %s (0x%04x)" % (RESP.get(rc, "?"), rc))
        self.log("session open")

    def device_info(self):
        rc, extra, data = self.op(0x1001, dataphase=1)
        if rc != 0x2001:
            raise RuntimeError("GetDeviceInfo -> %s (0x%04x)" % (RESP.get(rc, "?"), rc))
        return data

    @staticmethod
    def parse_device_info(d):
        o = 0
        o += 2 + 4 + 2               # StandardVersion, VendorExtID, VendorExtVersion
        n = d[o]; o += 1 + 2 * n     # VendorExtensionDesc
        o += 2                        # FunctionalMode
        cnt = struct.unpack_from("<I", d, o)[0]; o += 4
        ops = list(struct.unpack_from("<%dH" % cnt, d, o)); o += 2 * cnt
        return ops


def local_subnet():
    """Best-effort: return the /24 prefix this machine is on, e.g. '192.168.1.'."""
    import subprocess, re
    try:
        out = subprocess.run(["ifconfig"], capture_output=True, text=True, timeout=4).stdout
    except Exception:
        return None
    addrs = re.findall(r"inet (\d+\.\d+\.\d+)\.\d+ ", out)
    for a in addrs:
        if a.startswith("127."):
            continue
        return a + "."
    return None


def scan_subnet(prefix=None, port=15740, timeout=0.35, workers=64):
    """Find hosts with the PTP/IP port open. Slow-ish, but needs no mDNS."""
    import concurrent.futures, socket
    prefix = prefix or local_subnet()
    if not prefix:
        return []
    def probe_host(i):
        ip = "%s%d" % (prefix, i)
        s = socket.socket()
        s.settimeout(timeout)
        try:
            s.connect((ip, port))
            return ip
        except Exception:
            return None
        finally:
            s.close()
    found = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as ex:
        for r in ex.map(probe_host, range(1, 255)):
            if r:
                found.append(r)
    return found


def discover(timeout=3.0):
    """Find PTP/IP cameras. mDNS first (the 6D advertises _ptp._tcp), then a scan."""
    import subprocess
    ips = []
    if os.path.exists("/usr/bin/dns-sd"):
        try:
            p = subprocess.run(["dns-sd", "-B", "_ptp._tcp"],
                               capture_output=True, text=True, timeout=timeout)
            names = []
            for line in (p.stdout or "").splitlines():
                if "_ptp._tcp" in line:
                    names.append(line.split()[-1])
            for n in names:
                r = subprocess.run(["dns-sd", "-L", n, "_ptp._tcp"],
                                   capture_output=True, text=True, timeout=timeout)
                import re
                m = re.search(r"can be reached at ([\w.-]+)\.?:(\d+)", r.stdout or "")
                if not m:
                    continue
                host, port = m.group(1), m.group(2)
                g = subprocess.run(["dns-sd", "-G", "v4", host],
                                   capture_output=True, text=True, timeout=timeout)
                mm = re.search(r"(\d+\.\d+\.\d+\.\d+)", g.stdout or "")
                if mm:
                    ips.append((mm.group(1), int(port)))
        except Exception:
            pass
    if not ips:
        for ip in scan_subnet():
            ips.append((ip, 15740))
    return ips


def probe(ip):
    c = Client(ip)
    c.connect()
    c.init()
    c.open_session()
    d = c.device_info()
    ops = c.parse_device_info(d)
    print("supported operations: %d" % len(ops))
    for op in ops:
        print("  0x%04x %s" % (op, OPS.get(op, "")))
    up = [o for o in (0x100C, 0x100D) if o in ops]
    print("\nUPLOAD SUPPORTED" if len(up) == 2 else "\nNO upload op in the list")
    return ops


def object_info(name, size):
    """PTP ObjectInfo dataset (PIMA 15740), in the order the standard fixes."""
    d  = struct.pack("<I", 0xFFFFFFFF)   # StorageID: default
    d += struct.pack("<H", 0x3000)       # ObjectFormat: undefined
    d += struct.pack("<H", 0)            # ProtectionStatus
    d += struct.pack("<I", size)         # ObjectCompressedSize
    d += struct.pack("<H", 0)            # ThumbFormat
    d += struct.pack("<I", 0)            # ThumbCompressedSize
    d += struct.pack("<I", 0)            # ThumbPixWidth
    d += struct.pack("<I", 0)            # ThumbPixHeight
    d += struct.pack("<I", 0)            # ImagePixWidth
    d += struct.pack("<I", 0)            # ImagePixHeight
    d += struct.pack("<I", 0)            # ImageBitDepth
    d += struct.pack("<I", 0)            # ParentObject
    d += struct.pack("<H", 0)            # AssociationType: none
    d += struct.pack("<I", 0)            # AssociationDesc
    d += struct.pack("<I", 0)            # SequenceNumber
    d += ptp_string(name)                # Filename
    d += ptp_string("")                  # CaptureDate
    d += ptp_string("")                  # ModificationDate
    d += ptp_string("")                  # Keywords
    return d


def upload(ip, path, name=None):
    size = os.path.getsize(path)
    name = name or os.path.basename(path).upper()
    c = Client(ip)
    c.connect()
    c.init()
    c.open_session()
    rc, extra, _ = c.op(0x100C, [], data=object_info(name, size))
    print("SendObjectInfo -> %s (0x%04x)" % (RESP.get(rc, "?"), rc))
    if rc != 0x2001:
        print("   the body refused the object info; nothing was transferred")
        return rc
    storage, handle = (struct.unpack("<II", extra[:8])
                       if len(extra) >= 8 else (0xFFFFFFFF, 0))
    print("   storage 0x%08x handle 0x%08x" % (storage, handle))
    with open(path, "rb") as f:
        blob = f.read()
    rc, _, _ = c.op(0x100D, [handle], data=blob)
    print("SendObject -> %s (0x%04x)" % (RESP.get(rc, "?"), rc))
    return rc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", required=True)
    ap.add_argument("cmd", choices=["probe", "upload"])
    ap.add_argument("file", nargs="?")
    ap.add_argument("name", nargs="?")
    a = ap.parse_args()
    try:
        if a.cmd == "probe":
            probe(a.ip)
        else:
            if not a.file:
                print("upload needs a file"); sys.exit(2)
            upload(a.ip, a.file, a.name)
    except Exception as e:
        print("FAILED: %s" % e)
        sys.exit(1)

if __name__ == "__main__":
    main()
