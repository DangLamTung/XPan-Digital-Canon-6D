"""
Compose a two-column promo sheet of XPan Develop renders.

Each cell is a film-strip frame: a label bar above the picture, a one-pixel
light rule above and below it, on the same near-black background the earlier
single-column sheets used.

The frames are real engine output, not mock-ups: every picture is produced by
se_cam_test, the host harness that runs se/se_cam.c itself (the camera's own
develop path) at 65:24 framing, and is pasted in unscaled. A recipe variant is
expressed as extra lines appended to a shipped stock, so the variant always
tracks the current recipe instead of freezing a copy of it.

    python3 make_promo_sheet.py <cr2 dir> <out.png>

    STRENGTH=50 python3 ...       # the look at half strength, for comparison

Requires the host harness to be built:

    make -C magiclantern/modules/film_sim/se/hosttest
"""
import os
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFont

BG = (14, 14, 15)
BAR = (24, 24, 26)
RULE = (96, 94, 90)
TITLE = (200, 200, 198)
META = (110, 110, 112)
LABEL = (150, 150, 148)
LABEL_R = (104, 104, 106)

FRAME_W = 2334            # picture width; the single-column sheets used this
MARGIN = 33
GUTTER = 33
BAR_H = 37
HEADER_H = 100
ROW_GAP = 13              # background between one frame's rule and the next bar

FONT_CHOICES = [
    "/System/Library/Fonts/Supplemental/Courier New.ttf",
    "/System/Library/Fonts/Menlo.ttc",
]

HERE = os.path.dirname(os.path.abspath(__file__))
FILM_SIM = os.path.dirname(HERE)
FILMS = os.path.join(FILM_SIM, "films")
HOSTTEST = os.path.join(FILM_SIM, "se", "hosttest", "rawdev")

# (photo, stock, extra recipe lines, left label, right label)
ENTRIES = [
    ("IMG_9915.CR2", "classicneg.fs", "",
     "IMG_9915   FUJI CLASSIC NEGATIVE  ISO 125", "classicneg.fs   95%"),
    ("IMG_9938.CR2", "velvia.fs", "",
     "IMG_9938   FUJICHROME VELVIA 50   ISO 100", "velvia.fs   95%"),
    ("IMG_9931.CR2", "velvia.fs", "",
     "IMG_9931   FUJICHROME VELVIA 50   ISO 400", "velvia.fs   95%"),
    ("IMG_9934.CR2", "velvia.fs", "",
     "IMG_9934   FUJICHROME VELVIA 50   ISO 125", "velvia.fs   95%"),
    ("IMG_9920.CR2", "portra.fs", "",
     "IMG_9920   KODAK PORTRA 400       ISO 400", "portra.fs   95%"),
    ("IMG_0048.CR2", "soup800.fs", "",
     "IMG_0048   CINESTILL 800T NIGHT   ISO 6400", "soup800.fs   95%"),
]


def font(size):
    for path in FONT_CHOICES:
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    raise SystemExit("no monospace font found in %s" % FONT_CHOICES)


def variant(stock, extra, tmpdir, tag):
    """The shipped recipe, plus the variant's own lines. The parser assigns on
    each key, so a later duplicate overrides rather than merges."""
    base = os.path.join(FILMS, stock)
    if not extra:
        return base
    out = os.path.join(tmpdir, "v%s.fs" % tag)
    with open(base, "rb") as f:
        text = f.read()
    with open(out, "wb") as f:
        f.write(text)
        f.write(b"\n# variant for this sheet\n" + extra.encode() + b"\n")
    return out


def render(cr2, stock, strength, out_jpg):
    """One frame through the camera's own 15-bit CR2 develop path, at 65:24."""
    cmd = [HOSTTEST, cr2, stock, out_jpg, "2", "5", str(strength), "100"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out_jpg):
        raise SystemExit("render failed: %s\n%s%s" % (" ".join(cmd), r.stdout, r.stderr))
    for line in r.stdout.splitlines():
        if "recipe" in line:
            print("   " + line.strip())
    return Image.open(out_jpg).convert("RGB")


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    cr2dir, out_path = sys.argv[1], sys.argv[2]
    strength = int(os.environ.get("STRENGTH", "100"))
    tmp = tempfile.mkdtemp(prefix="sheet_")

    frames = []
    for i, (name, stock, extra, left, right) in enumerate(ENTRIES):
        recipe = variant(stock, extra, tmp, str(i))
        img = render(os.path.join(cr2dir, name), recipe,
                     strength, os.path.join(tmp, "f%d.jpg" % i))
        h = round(FRAME_W * img.size[1] / img.size[0])
        frames.append((img.resize((FRAME_W, h), Image.LANCZOS), left, right))
        print("%s  %dx%d" % (name, FRAME_W, h))

    frame_h = frames[0][0].size[1]
    for f, _, _ in frames:
        if f.size[1] != frame_h:
            raise SystemExit("frames disagree on height")

    cols = 2
    rows = (len(frames) + cols - 1) // cols
    row_pitch = BAR_H + 2 + frame_h + ROW_GAP
    w = MARGIN * 2 + FRAME_W * cols + GUTTER * (cols - 1)
    h = HEADER_H + rows * row_pitch - ROW_GAP + MARGIN

    sheet = Image.new("RGB", (w, h), BG)
    d = ImageDraw.Draw(sheet)
    ft, fb = font(30), font(23)

    d.text((MARGIN, 46), "FILM SIMULATION", font=ft, fill=TITLE)
    meta = "EOS 6D  ·  65:24 XPan  ·  strength %d%%  ·  developed from CR2" % strength
    d.text((w - MARGIN - d.textlength(meta, font=fb), 58), meta, font=fb, fill=META)

    asc, desc = fb.getmetrics()
    for i, (img, left, right) in enumerate(frames):
        r, c = divmod(i, cols)
        x = MARGIN + c * (FRAME_W + GUTTER)
        y = HEADER_H + r * row_pitch
        d.rectangle([x, y, x + FRAME_W - 1, y + BAR_H - 1], fill=BAR)
        ty = y + (BAR_H - asc - desc) // 2
        d.text((x + 13, ty), left, font=fb, fill=LABEL)
        d.text((x + FRAME_W - 13 - d.textlength(right, font=fb), ty), right,
               font=fb, fill=LABEL_R)
        d.line([x, y + BAR_H, x + FRAME_W - 1, y + BAR_H], fill=RULE)
        sheet.paste(img, (x, y + BAR_H + 1))
        d.line([x, y + BAR_H + 1 + frame_h, x + FRAME_W - 1, y + BAR_H + 1 + frame_h],
               fill=RULE)

    sheet.save(out_path)
    print("wrote %s  %dx%d" % (out_path, w, h))


if __name__ == "__main__":
    main()
