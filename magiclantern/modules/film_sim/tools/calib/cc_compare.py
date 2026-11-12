import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, r"C:\Users\Tung\Documents\Dehancer_Forensic_Evidence_2026-11-06\algorithm")
from compare_export import srgb_to_lab  # noqa: E402

COLS, ROWS, P, GAP = 6, 4, 160, 12


def patches(p):
    im = Image.open(p).convert("RGB")
    want = (COLS * P + (COLS + 1) * GAP, ROWS * P + (ROWS + 1) * GAP)
    if im.size != want:
        im = im.resize(want, Image.LANCZOS)
    a = np.asarray(im, dtype=np.float32)
    out = []
    for r in range(ROWS):
        for c in range(COLS):
            x = GAP + c * (P + GAP) + P // 2
            y = GAP + r * (P + GAP) + P // 2
            out.append(a[y - 40:y + 40, x - 40:x + 40].reshape(-1, 3).mean(axis=0))
    return np.array(out)


def de00(lab1, lab2):
    L1, a1, b1 = lab1
    L2, a2, b2 = lab2
    C1, C2 = np.hypot(a1, b1), np.hypot(a2, b2)
    Cb = (C1 + C2) / 2.0
    G = 0.5 * (1 - np.sqrt(Cb ** 7 / (Cb ** 7 + 25.0 ** 7)))
    a1p, a2p = a1 * (1 + G), a2 * (1 + G)
    C1p, C2p = np.hypot(a1p, b1), np.hypot(a2p, b2)
    h1p = np.degrees(np.arctan2(b1, a1p)) % 360
    h2p = np.degrees(np.arctan2(b2, a2p)) % 360
    dLp = L2 - L1
    dCp = C2p - C1p
    if C1p * C2p == 0:
        dhp = 0.0
    else:
        dhp = h2p - h1p
        if dhp > 180:
            dhp -= 360
        elif dhp < -180:
            dhp += 360
    dHp = 2 * np.sqrt(C1p * C2p) * np.sin(np.radians(dhp) / 2)
    Lb = (L1 + L2) / 2
    Cpb = (C1p + C2p) / 2
    if C1p * C2p == 0:
        hb = h1p + h2p
    elif abs(h1p - h2p) <= 180:
        hb = (h1p + h2p) / 2
    elif h1p + h2p < 360:
        hb = (h1p + h2p + 360) / 2
    else:
        hb = (h1p + h2p - 360) / 2
    T = (1 - 0.17 * np.cos(np.radians(hb - 30)) + 0.24 * np.cos(np.radians(2 * hb))
         + 0.32 * np.cos(np.radians(3 * hb + 6)) - 0.20 * np.cos(np.radians(4 * hb - 63)))
    SL = 1 + 0.015 * (Lb - 50) ** 2 / np.sqrt(20 + (Lb - 50) ** 2)
    SC = 1 + 0.045 * Cpb
    SH = 1 + 0.015 * Cpb * T
    RT = -2 * np.sqrt(Cpb ** 7 / (Cpb ** 7 + 25.0 ** 7)) * np.sin(np.radians(60 * np.exp(-((hb - 275) / 25) ** 2)))
    return np.sqrt((dLp / SL) ** 2 + (dCp / SC) ** 2 + (dHp / SH) ** 2 + RT * (dCp / SC) * (dHp / SH))


def main():
    root = Path(r"C:\Users\Tung\Documents\Dehancer_Forensic_Evidence_2026-11-06")
    ref = srgb_to_lab(patches(root / "chart_cc_deh" / "Cinestill-50D" / "cc.jpg").astype(np.float32))
    for tag, p in (("branch 50D.fs  (48-knot engine)", root / "chart_work" / "branch_cc.jpg"),
                   ("my 50D.fs      (96-knot engine)", root / "chart_work" / "mine_cc.jpg")):
        if not p.exists():
            print(f"{tag:34s} missing")
            continue
        ours = srgb_to_lab(patches(p).astype(np.float32))
        d = np.array([de00(ours[i], ref[i]) for i in range(len(ref))])
        names = [l.split()[1] for l in Path(r"E:\chart_cc\cc.txt").read_text().splitlines()]
        worst = np.argsort(-d)[:3]
        w = ", ".join(f"{names[i]} {d[i]:.1f}" for i in worst)
        print(f"{tag:34s} mean dE00 {d.mean():5.2f}  worst {d.max():5.1f}  ({w})")


if __name__ == "__main__":
    main()
