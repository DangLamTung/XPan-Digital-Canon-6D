#!/usr/bin/env python3
"""Share and install film-sim recipes over the internet, from the host side.

The camera cannot fetch from a network: its Wi-Fi runs Canon's PTP/IP server, and
ML has no socket API (see docs/wifi-functions.md). So the host does the network
part and the camera only ever reads files from ML/FILMS/.

    ./tools/recipe_online.py export  /Volumes/EOS_DIGITAL  pack.json
    ./tools/recipe_online.py pull    https://example.org/pack.json /Volumes/EOS_DIGITAL
    ./tools/recipe_online.py push    pack.json /Volumes/EOS_DIGITAL

A pack is JSON:

    {"format": "mlfilms-1", "recipes": [{"name": "...", "code": "P400", ...}, ...]}

Every recipe becomes one XML in ML/FILMS/, in the same format the module already
parses (film_xml.h), so "Reload Films" in the module picks it up with no reboot.
"""
import json, os, sys, urllib.request

RANGES = {
    "base": (0x80, 0x8F), "iso": (50, 3200), "raw_iso": (0x40, 0x78),
    "contrast": (-4, 4), "sharpness": (0, 7), "saturation": (-4, 4),
    "tone": (-4, 4), "kelvin": (4000, 6800), "wbs_ba": (-9, 9),
    "wbs_gm": (-9, 9), "mono": (0, 1),
}
REQUIRED = ("name", "base", "iso", "kelvin")

def parse_xml(text):
    import re
    out = {}
    for tag in ["name"] + list(RANGES) + ["code", "brand", "type", "lut", "description"]:
        m = re.search(r"<%s>(.*?)</%s>" % (tag, tag), text, re.S)
        if m:
            val = m.group(1).strip()
            out[tag] = val if tag in ("name", "code", "brand", "type", "lut", "description") else int(val, 0)
    return out

def to_xml(r):
    lines = ['<?xml version="1.0" encoding="UTF-8"?>', "<film>"]
    order = ["name", "code", "brand", "type", "base", "iso", "raw_iso", "contrast",
             "sharpness", "saturation", "tone", "kelvin", "wbs_ba", "wbs_gm", "mono",
             "lut", "description"]
    for k in order:
        if k in r and r[k] is not None:
            v = r[k]
            if isinstance(v, int):
                v = hex(v) if k in ("base", "raw_iso") else v
            lines.append("    <%s>%s</%s>" % (k, v, k))
    lines.append("</film>")
    return "\n".join(lines) + "\n"

def check(r, where):
    for k in REQUIRED:
        if k not in r:
            raise SystemExit("%s: missing <%s>" % (where, k))
    for k, (lo, hi) in RANGES.items():
        if k in r and not (lo <= r[k] <= hi):
            raise SystemExit("%s: %s=%s outside %s..%s" % (where, k, r[k], lo, hi))
    for k in ("name", "code", "brand", "type", "description"):
        if k in r and len(str(r[k])) > {"name": 31, "code": 11, "brand": 15,
                                        "type": 23, "description": 79}[k]:
            raise SystemExit("%s: %s too long for the module's field" % (where, k))

def card_films(mount):
    d = os.path.join(mount, "ML", "FILMS")
    if not os.path.isdir(d):
        d = os.path.join(mount, "ML", "FILMS")
    return d

def cmd_export(mount, out):
    d = card_films(mount)
    recipes = []
    for fn in sorted(os.listdir(d)):
        if not fn.lower().endswith(".xml"):
            continue
        recipes.append(parse_xml(open(os.path.join(d, fn), encoding="utf-8", errors="ignore").read()))
    json.dump({"format": "mlfilms-1", "recipes": recipes}, open(out, "w"), indent=1)
    print("exported %d recipes -> %s" % (len(recipes), out))

def install(pack, mount):
    d = card_films(mount)
    os.makedirs(d, exist_ok=True)
    n = 0
    for r in pack["recipes"]:
        check(r, r.get("name", "?"))
        fn = os.path.join(d, "ONL_%s.xml" % (r.get("code") or r["name"])[:10].replace(" ", ""))
        open(fn, "w", encoding="utf-8").write(to_xml(r))
        n += 1
    print("installed %d recipes into %s" % (n, d))

def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(1)
    cmd = sys.argv[1]
    if cmd == "export":
        cmd_export(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else "pack.json")
    elif cmd == "pull":
        data = urllib.request.urlopen(sys.argv[2], timeout=20).read()
        install(json.loads(data), sys.argv[3])
    elif cmd == "push":
        install(json.load(open(sys.argv[2])), sys.argv[3])
    else:
        print(__doc__); sys.exit(1)

if __name__ == "__main__":
    main()
