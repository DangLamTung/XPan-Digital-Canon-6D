#!/usr/bin/env python3
"""Catch the silent module failures that a green build does not.

Three bugs shipped in this repository's own modules, all of which compiled:

  1. a property handler defined but never listed in MODULE_PROPHANDLERS,
     so the module loaded and never fired;
  2. a parallel array shorter than MLW_NIDS, so the watcher read a NULL
     pointer -- C zero-fills the rest and the compiler says nothing;
  3. file I/O inside a property handler, which runs in PropMgrTask.

Usage: ./tools/modcheck.py magiclantern/modules/mlwifi magiclantern/modules/aftrace
"""
import re, sys, os

SYM = "magiclantern/platform/6D.116/build/6D_116.sym"
IO_IN_HANDLER = ("FIO_", "msleep", "NotifyBox", "task_create", "bmp_printf",
                 "printf", "snprintf", "vsnprintf")


def handler_bodies(src):
    """Yield (name, body) for every PROP_HANDLER(...) { ... } block."""
    for m in re.finditer(r"PROP_HANDLER\(([^)]*)\)\s*\{", src):
        i, depth = m.end(), 1
        while i < len(src) and depth:
            depth += {"{": 1, "}": -1}.get(src[i], 0)
            i += 1
        yield m.group(1).strip(), src[m.end():i - 1]


def check(mod):
    c = os.path.join(mod, os.path.basename(mod) + ".c")
    if not os.path.exists(c):
        c = next((os.path.join(mod, f) for f in os.listdir(mod)
                  if f.endswith(".c")), None)
    if not c:
        return print(f"{mod}: no .c file"), 0
    src = open(c).read()
    problems = []

    nids = re.search(r"#define MLW_NIDS (\d+)u", src)
    if nids:
        nids = int(nids.group(1))
        counts = {
            "handlers": len(re.findall(r"MLW_PROP_INT\(0x", src)),
            "ptrs": len(re.findall(r"&mlw_v\d+", src)),
            "ids": len(re.findall(r"0x[0-9A-Fa-f]+u",
                                  (re.search(r"mlw_ids\[MLW_NIDS\]\s*=\s*\{(.*?)\};",
                                             src, re.S) or [None, ""])[1])),
            "registered": len(re.findall(r"MODULE_PROPHANDLER\(0x", src)),
        }
        for k, v in counts.items():
            if v != nids:
                problems.append(f"MLW_NIDS={nids} but {k}={v}")

    for name, body in handler_bodies(src):
        for tok in IO_IN_HANDLER:
            if tok in body:
                problems.append(f"handler {name} calls {tok} (runs in PropMgrTask)")

    dep = os.path.join(mod, "build", os.path.basename(mod) + ".dep")
    if os.path.exists(dep) and os.path.exists(SYM):
        sym = open(SYM).read()
        for s in open(dep).read().split():
            if not re.search(r" " + re.escape(s) + r"$", sym, re.M):
                problems.append(f"import {s} is not in 6D_116.sym")

    print(f"{os.path.basename(mod)}: {len(problems)} problem(s)")
    for p in problems:
        print("   " + p)
    return None, len(problems)


def main():
    mods = sys.argv[1:]
    if not mods:
        print(__doc__)
        sys.exit(1)
    total = 0
    for m in mods:
        _, n = check(m)
        total += n
    sys.exit(1 if total else 0)


if __name__ == "__main__":
    main()
