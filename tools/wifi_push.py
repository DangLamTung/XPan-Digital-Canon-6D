#!/usr/bin/env python3
"""Push a file to the camera over Canon's own Wi-Fi (PTP/IP), then import it.

The camera is the PTP/IP **server**: mDNS service `_ptp._tcp`, TXT record
`tid.canon.com=<GUID>`, port 15740. A host authenticates with a 16-byte GUID;
libgphoto2 implements that, so this script drives gphoto2 rather than speaking
PTP itself.

Why this can work at all: the 6D firmware implements the PTP operations a host
needs to *send* a file -- `SendObjectInfo` (0x100C) and `SendObject` (0x100D) are
in the operation table at 0xFF1C313C/0xFF1C3144 with handlers at 0xFF281CA4 /
0xFF281854. Whether the Wi-Fi transport allows them here is not yet proven; this
script exists to find out.

    ./tools/wifi_push.py --ip 192.168.1.2 recipe.xml
    ./tools/wifi_push.py --ip 192.168.1.2 --list

On the camera afterwards: Film Dev -> Debug -> Import Recipes.
"""
import argparse, os, shutil, subprocess, sys

def have_gphoto2():
    return shutil.which("gphoto2") is not None

def run(args):
    print("+", " ".join(args))
    p = subprocess.run(args, capture_output=True, text=True)
    out = (p.stdout or "") + (p.stderr or "")
    return p.returncode, out.strip()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file", nargs="?", help="file to upload")
    ap.add_argument("--ip", help="camera IP (shown by the camera or its router)")
    ap.add_argument("--name", help="name to store it as on the camera")
    ap.add_argument("--list", action="store_true", help="just list files")
    a = ap.parse_args()

    if not have_gphoto2():
        print("gphoto2 is not installed. On macOS:\n\n    brew install gphoto2\n")
        sys.exit(2)
    if not a.ip:
        print("need --ip <camera address>; the camera shows it in its Wi-Fi screen")
        sys.exit(2)

    port = "ptpip:" + a.ip
    if a.list or not a.file:
        rc, out = run(["gphoto2", "--port", port, "--summary"])
        print(out or "(no output)")
        sys.exit(rc)

    name = a.name or os.path.basename(a.file).upper()
    rc, out = run(["gphoto2", "--port", port,
                   "--upload-file", a.file, "--filename", name])
    print(out or "(no output)")
    if rc == 0:
        print("\nuploaded as %s\nOn the camera: Film Dev -> Debug -> Import Recipes" % name)
    else:
        print("\ngphoto2 refused the upload (exit %d).\n"
              "If the message is about SendObject or 'not supported', the body\n"
              "does not accept uploads over PTP/IP and the file has to arrive on\n"
              "the card another way (card reader, or a Wi-Fi SD card)." % rc)
    sys.exit(rc)

if __name__ == "__main__":
    main()
