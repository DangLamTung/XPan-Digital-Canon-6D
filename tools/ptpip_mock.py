#!/usr/bin/env python3
"""A fake 6D speaking PTP/IP, so the client and the web panel can be tested here.

    ./tools/ptpip_mock.py            # listens on 127.0.0.1:15740

It implements the handshake, OpenSession, GetDeviceInfo (advertising the
operations the real body's table lists), and a tiny object store: two JPEGs and a
recipe XML. SendObjectInfo/SendObject write into /tmp/ptpip_mock_upload.

This exists because the real camera is not reachable from this machine; it proves
the client speaks the protocol, not that the camera accepts it.
"""
import base64, os, socket, socketserver, struct, sys, threading

PORT = 15740
UP = "/tmp/ptpip_mock_upload"
GUID = bytes(range(16))
NAME = "MOCK 6D"
PROTO = 0x00010000

INIT_CMD_REQ, INIT_CMD_ACK, INIT_EVT_REQ, INIT_EVT_ACK, INIT_FAIL = 1, 2, 3, 4, 5
OP_REQ, OP_RESP, EVENT, START_DATA, DATA, CANCEL, END_DATA = 6, 7, 8, 9, 10, 11, 12

OPS = [0x1001, 0x1002, 0x1003, 0x1004, 0x1005, 0x1006, 0x1007, 0x1008, 0x1009,
       0x100A, 0x100B, 0x100C, 0x100D, 0x100E, 0x100F, 0x1014, 0x1015, 0x1016,
       0x101B, 0x9104, 0x9105, 0x9106, 0x9107]

# a 1x1 red jpeg, enough for a thumbnail
JPEG = base64.b64decode(
    b"/9j/4AAQSkZJRgABAQEAYABgAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRof"
    b"Hh0aHBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/wAALCAABAAEBAREA/8QAFAAB"
    b"AAAAAAAAAAAAAAAAAAAACf/EABQQAQAAAAAAAAAAAAAAAAAAAAD/2gAIAQEAAD8AKp//2Q==")

FILES = {
    1001: ("IMG_0001.JPG", JPEG, 0x3801, "20261115T120000"),
    1002: ("IMG_0002.JPG", JPEG + b"pad", 0x3801, "20261115T120500"),
    1003: ("RECIPE.XML", b"<film><name>Mock</name><base>0x84</base><iso>400</iso>"
                        b"<kelvin>5200</kelvin></film>\n", 0x3000, "20261115T121000"),
}


def s16(t):
    b = t.encode("utf-16-le") + b"\x00\x00"
    return bytes([len(b) // 2]) + b


def object_info(handle):
    name, data, fmt, date = FILES[handle]
    d  = struct.pack("<I", 0x00010001)
    d += struct.pack("<H", fmt)
    d += struct.pack("<H", 0)
    d += struct.pack("<I", len(data))
    d += struct.pack("<H", 0) + struct.pack("<I", 0)
    d += struct.pack("<I", 0) + struct.pack("<I", 0)
    d += struct.pack("<I", 1) + struct.pack("<I", 1) + struct.pack("<I", 24)
    d += struct.pack("<I", 0) + struct.pack("<H", 0)
    d += struct.pack("<I", 0) + struct.pack("<I", 0)
    d += s16(name) + s16(date) + s16("") + s16("")
    return d


def device_info():
    d  = struct.pack("<H", 100)              # StandardVersion
    d += struct.pack("<I", 0x00000006)       # VendorExtensionID (Canon)
    d += struct.pack("<H", 100)              # VendorExtensionVersion
    d += s16("Canon")                        # VendorExtensionDesc
    d += struct.pack("<H", 0)                # FunctionalMode
    d += struct.pack("<I", len(OPS)) + struct.pack("<%dH" % len(OPS), *OPS)
    d += struct.pack("<I", 0)                # EventsSupported
    d += struct.pack("<I", 0)                # DevicePropertiesSupported
    d += struct.pack("<I", 0)                # CaptureFormats
    d += struct.pack("<I", 0)                # ImageFormats
    d += s16("Canon Inc.") + s16(NAME) + s16("1.0.0") + s16("000000000001")
    return d


class Handler(socketserver.BaseRequestHandler):
    def send(self, ptype, body=b""):
        self.request.sendall(struct.pack("<II", 8 + len(body), ptype) + body)

    def recv_packet(self):
        head = b""
        while len(head) < 8:
            c = self.request.recv(8 - len(head))
            if not c:
                return None, None
            head += c
        length, ptype = struct.unpack("<II", head)
        body = b""
        while len(body) < length - 8:
            c = self.request.recv(length - 8 - len(body))
            if not c:
                return None, None
            body += c
        return ptype, body

    def handle(self):
        conn = 1
        while True:
            ptype, body = self.recv_packet()
            if ptype is None:
                return
            if ptype == INIT_CMD_REQ:
                self.send(INIT_CMD_ACK, struct.pack("<I", conn) + GUID + s16(NAME)
                          + struct.pack("<I", PROTO))
            elif ptype == INIT_EVT_REQ:
                self.send(INIT_EVT_ACK)
            elif ptype == OP_REQ:
                self.op(body)
            elif ptype in (START_DATA, DATA, END_DATA):
                continue

    def op(self, body):
        dataphase, code, tid = struct.unpack("<IHI", body[:10])
        params = [struct.unpack_from("<I", body, 10 + 4 * i)[0]
                  for i in range((len(body) - 10) // 4)]
        rcode, rparams, data = 0x2001, [], b""
        if code == 0x1002:
            pass
        elif code == 0x1001:
            data = device_info()
        elif code == 0x1004:
            data = struct.pack("<II", 1, 0x00010001)
        elif code == 0x1007:
            hs = list(FILES)
            data = struct.pack("<I", len(hs)) + struct.pack("<%dI" % len(hs), *hs)
        elif code == 0x1008:
            data = object_info(params[0]) if params and params[0] in FILES else b""
            if not data:
                rcode = 0x200A
        elif code == 0x100A:
            data = FILES[params[0]][1] if params and params[0] in FILES else b""
            if not data:
                rcode = 0x200A
        elif code == 0x1009:
            data = FILES[params[0]][1] if params and params[0] in FILES else b""
            if not data:
                rcode = 0x200A
        elif code == 0x100E:
            pass
        elif code == 0x100C:
            # the ObjectInfo arrives in the data phase, which we swallow here
            rparams = [0x00010001, 2000]
        elif code == 0x100D:
            pass
        elif code == 0x1003:
            pass
        else:
            rcode = 0x2005
        if data:
            self.send(START_DATA, struct.pack("<IQ", tid, len(data)))
            self.send(DATA, struct.pack("<I", tid) + data)
            self.send(END_DATA, struct.pack("<I", tid))
        resp = struct.pack("<HI", rcode, tid) + b"".join(struct.pack("<I", p) for p in rparams)
        self.send(OP_RESP, resp)


class S(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    os.makedirs(UP, exist_ok=True)
    print("mock 6D on 127.0.0.1:%d" % PORT)
    S(("127.0.0.1", PORT), Handler).serve_forever()
