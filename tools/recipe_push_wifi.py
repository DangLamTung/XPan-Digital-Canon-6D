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
    ap.add_argument("--ip", required=True)
    ap.add_argument("--port", type=int, default=15740)
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
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
