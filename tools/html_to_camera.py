#!/usr/bin/env python3
"""Render an HTML page and put it on the card so the camera can display it.

The camera cannot serve HTML and cannot render it: ML has no socket API for the
former, and no browser engine for the latter. What it *can* do is show a bitmap
from the card -- `pic_view` handles BMP (src/bmp.h: bits_per_pixel 1/4/8/24,
compression 0) and scales it to 720x480.

So the page is rendered on the host by macOS QuickLook, turned into a 24-bit BMP
at exactly the screen size, and written to ML/HTML/ on the card. On the camera:
file manager -> ML/HTML -> the .BMP -> View.

    ./tools/html_to_camera.py page.html /Volumes/EOS_DIGITAL [NAME]

Any HTML the system can render works, including images and CSS; scripts run at
render time only, and nothing is interactive on the camera.
"""
import os, shutil, subprocess, sys, tempfile

W, H = 720, 480

def render_png(html_path, outdir):
    if not shutil.which("qlmanage"):
        raise SystemExit("qlmanage not found (macOS only)")
    r = subprocess.run(["qlmanage", "-t", "-s", "1440", "-o", outdir, html_path],
                       capture_output=True, text=True)
    for fn in os.listdir(outdir):
        if fn.lower().endswith(".png"):
            return os.path.join(outdir, fn)
    raise SystemExit("QuickLook produced no thumbnail:\n" + (r.stderr or r.stdout))

def to_bmp(png_path, bmp_path):
    from PIL import Image
    im = Image.open(png_path).convert("RGB")
    # fit inside 720x480, keep aspect, black letterbox
    k = min(W / im.width, H / im.height)
    nw, nh = max(1, int(im.width * k)), max(1, int(im.height * k))
    im = im.resize((nw, nh), Image.LANCZOS)
    canvas = Image.new("RGB", (W, H), (0, 0, 0))
    canvas.paste(im, ((W - nw) // 2, (H - nh) // 2))
    canvas.save(bmp_path, "BMP")     # PIL writes 24-bit uncompressed for RGB
    return bmp_path

def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    html_path = sys.argv[1]
    mount = sys.argv[2] if len(sys.argv) > 2 else None
    name = (sys.argv[3] if len(sys.argv) > 3 else
            os.path.splitext(os.path.basename(html_path))[0]).upper()
    if not os.path.isfile(html_path):
        raise SystemExit("no such file: %s" % html_path)

    with tempfile.TemporaryDirectory() as td:
        png = render_png(os.path.abspath(html_path), td)
        bmp = to_bmp(png, os.path.join(td, name + ".BMP"))
        size = os.path.getsize(bmp)
        print("rendered %s -> %dx%d BMP, %d bytes" % (os.path.basename(html_path), W, H, size))
        if mount:
            dst_dir = os.path.join(mount, "ML", "HTML")
            os.makedirs(dst_dir, exist_ok=True)
            dst = os.path.join(dst_dir, name + ".BMP")
            shutil.copyfile(bmp, dst)
            print("installed", dst)
            print("on the camera: file manager -> ML/HTML -> %s.BMP -> View" % name)
        else:
            out = os.path.abspath(name + ".BMP")
            shutil.copyfile(bmp, out)
            print("wrote", out, "(pass a card mount to install it)")

if __name__ == "__main__":
    main()
