#!/usr/bin/env python3
"""Push recipe files into the camera over Canon's own WiFi stack (PTP/IP).

The 6D cannot host a socket in ML -- its network layer lives past the end of the
ROM image this project has -- but Canon's own PTP/IP server does run whenever
Wi-Fi is on in the camera menu. So the recipe travels the way Canon already
knows how: SendObject into the card, then the module's Import picks it up.

    ./tools/recipe_push_wifi.py --ip 192.168.1.42 ML/FILMS/portra.fs
    ./tools/recipe_push_wifi.py --ip 192.168.1.42 films/gold.fs samples/rain-street.fs

After it reports OK, on the camera:
    Film Dev > Film Settings > Debug > Import Recipes
which moves the files out of DCIM into ML/FILM and reloads the shelf.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ptpip


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", help="camera address; omit to discover it")
    ap.add_argument("--port", type=int, default=15740)
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()

    if not a.ip:
        print("looking for a PTP/IP camera...")
        ips = ptpip.discover()
        if not ips:
            print("no camera answered mDNS; scanning the subnet")
            ips = ptpip.scan_subnet()
        if not ips:
            # a loopback camera is what the mock serves for tests
            try:
                import socket
                socket.create_connection(("127.0.0.1", a.port), 0.4).close()
                ips = ["127.0.0.1"]
            except Exception:
                pass
        if not ips:
            print("no camera found. Turn Wi-Fi on (Connect to computer) and retry,")
            print("or pass --ip with the address the camera shows.")
            sys.exit(1)
        a.ip = ips[0]
        print("using", a.ip)

    ok = 0
    for path in a.files:
        name = os.path.basename(path)
        try:
            r = ptpip.upload(a.ip, path, name)
        except Exception as e:
            print("  %-24s FAILED: %s" % (name, e))
            continue
        print("  %-24s sent" % name)
        ok += 1
    print("%d/%d sent to %s" % (ok, len(a.files), a.ip))
    if ok:
        print("now: Film Dev > Film Settings > Debug > Import Recipes")


if __name__ == "__main__":
    main()
