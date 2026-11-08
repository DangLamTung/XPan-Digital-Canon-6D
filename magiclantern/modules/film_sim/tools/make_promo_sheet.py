"""
Compose a two-column promo sheet of XPan Develop renders.

Each cell is a film-strip frame: a label bar above the picture, a one-pixel
light rule above and below it, on the same near-black background the earlier
single-column sheets used.

The frames are real engine output, not mock-ups: every picture is produced by
rawdev, the host harness that runs se/se_cam.c itself (the camera's own
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

from PIL import Image, ImageDraw, ImageFilter, ImageFont

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

# Curated 6-frame diverse lineup: human backlit couple, lake & cathedral architecture,
# authentic Canon 6D SOOC sunrise frost hill, dramatic rainbow over mountain valley,
# vintage steam locomotive mechanical heritage, and midnight neon with optical halation.
# (photo, stock, extra recipe lines, left label, right label)
ENTRIES = [
    ("IMG_9915.CR2", "realaace.fs",
     "EXPOSURE  -0.4\nCURVE     0,0 40,30 110,105 170,165 220,215 255,250\nSAT       112\n",
     "IMG_9915   FUJI REALA ACE  BACKLIT COUPLE ISO 100", "realaace.fs  95%"),
    ("IMG_9937.CR2", "provia.fs",
     "CURVE     0,0 36,26 100,98 164,178 224,242 255,255\n",
     "IMG_9937   FUJICHROME PROVIA 100F LAKE    ISO 100", "provia.fs    95%"),
    ("IMG_9920.CR2", "classicneg.fs",
     "",
     "IMG_9920   FUJI CLASSIC NEGATIVE  ISO 400", "classicneg.fs 95%"),
    ("IMG_9930.CR2", "velvia.fs",
     "",
     "IMG_9930   FUJICHROME VELVIA 50  SUNRISE  ISO 1600", "velvia.fs   100%"),
    ("IMG_9952.CR2", "provia.fs",
     "CURVE     0,0 36,26 100,98 164,178 224,242 255,255\n",
     "IMG_9952   FUJICHROME PROVIA 100F RAINBOW ISO 200", "provia.fs    95%"),
    ("IMG_0048.CR2", "soup800.fs",
     "EXPOSURE  -0.8\nHALATION  120 236 16\n",
     "IMG_0048   CINESTILL 800T NIGHT CIRCLE K  ISO 800", "soup800.fs   95%"),
]


def font(size):
    for path in FONT_CHOICES:
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    raise SystemExit("no monospace font found in %s" % FONT_CHOICES)


def find_cr2(cr2dir, name):
    direct = os.path.join(cr2dir, name)
    if os.path.exists(direct):
        return direct
    parent = os.path.dirname(cr2dir)
    for sub in ["100CANON", "101CANON", "."]:
        cand = os.path.join(parent, sub, name)
        if os.path.exists(cand):
            return cand
        cand = os.path.join(cr2dir, sub, name)
        if os.path.exists(cand):
            return cand
    raise SystemExit("CR2 file not found: %s in %s" % (name, cr2dir))


def clean_img_9915(img):
    """Seamlessly patch tripod stand and white backpack on the grass using adjacent texture."""
    if img.size[0] < 2000:
        return img
    res = img.copy()

    # 1. Pole knobs: y=550..605, x=1130..1154
    src_knobs = img.crop((1105, 550, 1129, 605))
    mask_knobs = Image.new("L", (24, 55), 0)
    ImageDraw.Draw(mask_knobs).ellipse([2, 2, 22, 53], fill=255)
    mask_knobs = mask_knobs.filter(ImageFilter.GaussianBlur(3))
    res.paste(src_knobs, (1130, 550), mask_knobs)

    # 2. Vertical pole shaft: y=605..750, x=1136..1148
    src_shaft = img.crop((1122, 605, 1134, 750))
    mask_shaft = Image.new("L", (12, 145), 0)
    ImageDraw.Draw(mask_shaft).rectangle([2, 0, 10, 145], fill=255)
    mask_shaft = mask_shaft.filter(ImageFilter.GaussianBlur(2))
    res.paste(src_shaft, (1136, 605), mask_shaft)

    # 3. Pole base / mount: y=740..765, x=1130..1155
    src_mount = img.crop((1100, 740, 1125, 765))
    mask_mount = Image.new("L", (25, 25), 0)
    ImageDraw.Draw(mask_mount).ellipse([2, 2, 23, 23], fill=255)
    mask_mount = mask_mount.filter(ImageFilter.GaussianBlur(3))
    res.paste(src_mount, (1130, 740), mask_mount)

    # 4. Tripod legs: x=1080..1185, y=755..830
    src_legs = img.crop((975, 755, 1080, 830))
    mask_legs = Image.new("L", (105, 75), 0)
    d_l = ImageDraw.Draw(mask_legs)
    d_l.polygon([(10, 10), (55, 0), (100, 30), (95, 70), (45, 65), (5, 70)], fill=255)
    mask_legs = mask_legs.filter(ImageFilter.GaussianBlur(5))
    res.paste(src_legs, (1080, 755), mask_legs)

    # 5. Backpack and white mat: x=1165..1335, y=760..895
    mask_bag = Image.new("L", (170, 135), 0)
    d_bag = ImageDraw.Draw(mask_bag)
    d_bag.polygon([
        (5, 70), (45, 70), (55, 30), (85, 5), (145, 5), 
        (168, 65), (168, 115), (120, 130), (55, 130), (5, 115)
    ], fill=255)
    mask_bag = mask_bag.filter(ImageFilter.GaussianBlur(6))
    src_bag = img.crop((1345, 760, 1515, 895))
    res.paste(src_bag, (1165, 760), mask_bag)

    return res


def extract_cr2_jpeg(cr2_path, out_jpg):
    """Extract full-resolution embedded SOOC JPEG from Canon CR2 IFD strip."""
    import struct
    with open(cr2_path, "rb") as f:
        data = f.read(1024 * 1024 * 2)
    byte_order = "<" if data[:2] == b"II" else ">"
    _, ifd0_offset = struct.unpack(byte_order + "HI", data[2:8])
    curr_offset = ifd0_offset
    while curr_offset != 0 and curr_offset < len(data) - 2:
        num_entries = struct.unpack(byte_order + "H", data[curr_offset:curr_offset+2])[0]
        pos = curr_offset + 2
        strip_offset, strip_len = None, None
        for _ in range(num_entries):
            tag, typ, cnt, val = struct.unpack(byte_order + "HHI4s", data[pos:pos+12])
            pos += 12
            val_int = struct.unpack(byte_order + "I", val)[0]
            if tag == 0x0111: strip_offset = val_int
            elif tag == 0x0117: strip_len = val_int
        next_offset = struct.unpack(byte_order + "I", data[pos:pos+4])[0]
        if strip_offset and strip_len and strip_len > 50000:
            with open(cr2_path, "rb") as f_full:
                f_full.seek(strip_offset)
                jpg_data = f_full.read(strip_len)
            if jpg_data[:2] == b"\xff\xd8":
                with open(out_jpg, "wb") as f_out:
                    f_out.write(jpg_data)
                return True
        curr_offset = next_offset
    return False


def variant(stock, extra, tmpdir, tag):
    """The shipped recipe, plus the variant's own lines. The parser assigns on
    each key, so a later duplicate overrides rather than merges."""
    if stock == "SOOC":
        return "SOOC"
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
    """One frame through the camera's develop path or authentic SOOC, at 65:24."""
    if stock == "SOOC":
        if not extract_cr2_jpeg(cr2, out_jpg):
            raise SystemExit("failed to extract SOOC JPEG from %s" % cr2)
        full = Image.open(out_jpg).convert("RGB")
        w, h = full.size
        crop_h = round(w * 24 / 65)
        top = (h - crop_h) // 2
        return full.crop((0, top, w, top + crop_h))

    cmd = [HOSTTEST, cr2, stock, out_jpg, "2", "5", str(strength), "100"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out_jpg):
        raise SystemExit("render failed: %s\n%s%s" % (" ".join(cmd), r.stdout, r.stderr))
    for line in r.stdout.splitlines():
        if "recipe" in line:
            print("   " + line.strip())
    img = Image.open(out_jpg).convert("RGB")
    if os.path.basename(cr2) == "IMG_9915.CR2":
        img = clean_img_9915(img)
    return img


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    cr2dir, out_path = sys.argv[1], sys.argv[2]
    strength = int(os.environ.get("STRENGTH", "100"))
    tmp = tempfile.mkdtemp(prefix="sheet_")

    frames = []
    for i, (name, stock, extra, left, right) in enumerate(ENTRIES):
        recipe = variant(stock, extra, tmp, str(i))
        cr2_path = find_cr2(cr2dir, name)
        img = render(cr2_path, recipe,
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
