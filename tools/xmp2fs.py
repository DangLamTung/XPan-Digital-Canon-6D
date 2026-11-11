#!/usr/bin/env python3
"""
Convert Adobe Lightroom / Camera Raw XMP presets to Magic Lantern .fs recipes
using Soup Engine's Lightroom-aligned 8-band HSL mode (HSL 1).
"""
import sys
import os
import re
import math

BANDS = ["Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta"]
BAND_HUES = [0, 30, 60, 120, 180, 240, 270, 300]

def hsl_to_rgb_delta(hue_deg, sat_pct, max_gain=15.0):
    if sat_pct <= 0:
        return 0, 0, 0
    rad = math.radians(hue_deg)
    # Chroma vector in normalized space
    gain = (sat_pct / 100.0) * max_gain
    # Hue 0=Red, 120=Green, 240=Blue
    r = gain * math.cos(rad)
    g = gain * math.cos(rad - 2.0 * math.pi / 3.0)
    b = gain * math.cos(rad - 4.0 * math.pi / 3.0)
    # Center around zero
    mean = (r + g + b) / 3.0
    return int(round(r - mean)), int(round(g - mean)), int(round(b - mean))

def convert_xmp(xmp_path, skin_warmth_fix=True):
    with open(xmp_path, 'r', encoding='utf-8', errors='ignore') as f:
        txt = f.read()

    name_m = re.search(r'crs:Name>.*?<rdf:li[^>]*>(.*?)</rdf:li>', txt, re.DOTALL)
    if name_m:
        name = name_m.group(1).strip()
    else:
        name = os.path.splitext(os.path.basename(xmp_path))[0].replace(' parvec', '')

    # Curves
    curve_pts = []
    tc_m = re.search(r'crs:ToneCurvePV2012>.*?<rdf:Seq>(.*?)</rdf:Seq>', txt, re.DOTALL)
    if tc_m:
        raw_pts = re.findall(r'<rdf:li>(.*?)</rdf:li>', tc_m.group(1))
        for p in raw_pts:
            parts = p.split(',')
            if len(parts) == 2:
                curve_pts.append(f"{parts[0].strip()},{parts[1].strip()}")
    if not curve_pts:
        curve_pts = ["0,0", "255,255"]

    # HSL
    hue_adj = []
    sat_adj = []
    lum_adj = []
    for b in BANDS:
        hm = re.search(rf'crs:HueAdjustment{b}="([+-]?\d+)"', txt)
        sm = re.search(rf'crs:SaturationAdjustment{b}="([+-]?\d+)"', txt)
        lm = re.search(rf'crs:LuminanceAdjustment{b}="([+-]?\d+)"', txt)
        h_val = float(hm.group(1)) if hm else 0.0
        s_val = float(sm.group(1)) if sm else 0.0
        l_val = float(lm.group(1)) if lm else 0.0
        hue_adj.append(h_val)
        sat_adj.append(s_val)
        lum_adj.append(l_val)

    # In LR, +/-100 maps to +/-30 deg
    hue_rot = [round(h * 0.30, 1) for h in hue_adj]
    hue_sat = [int(round(100.0 + s)) for s in sat_adj]
    hue_lum = [int(round(100.0 + l)) for l in lum_adj]

    # Skin color fix:
    # If the user notices skin is too yellow, prevent positive orange/yellow rotation
    # and pull yellow saturation down.
    if skin_warmth_fix:
        # Band 1 is Orange, Band 2 is Yellow
        # Shift Orange slightly towards Red/Peach (-6 deg) to kill yellow mask
        if hue_rot[1] > -4.0:
            hue_rot[1] = -6.0
        if hue_rot[2] > -2.0:
            hue_rot[2] = -2.0
        # Ensure yellow saturation is restrained
        if hue_sat[2] > 80:
            hue_sat[2] = 78

    # Split Toning / Color Grading
    sh_hue_m = re.search(r'crs:SplitToningShadowHue="(\d+)"', txt)
    sh_sat_m = re.search(r'crs:SplitToningShadowSaturation="(\d+)"', txt)
    hi_hue_m = re.search(r'crs:SplitToningHighlightHue="(\d+)"', txt)
    hi_sat_m = re.search(r'crs:SplitToningHighlightSaturation="(\d+)"', txt)
    
    sh_h = float(sh_hue_m.group(1)) if sh_hue_m else 0.0
    sh_s = float(sh_sat_m.group(1)) if sh_sat_m else 0.0
    hi_h = float(hi_hue_m.group(1)) if hi_hue_m else 0.0
    hi_s = float(hi_sat_m.group(1)) if hi_sat_m else 0.0

    # Also check ColorGradeMidtone
    mid_hue_m = re.search(r'crs:ColorGradeMidtoneHue="(\d+)"', txt)
    mid_sat_m = re.search(r'crs:ColorGradeMidtoneSat="(\d+)"', txt)
    if mid_hue_m and mid_sat_m:
        # Blend midtone warmth into shadows/highlights
        mh = float(mid_hue_m.group(1))
        ms = float(mid_sat_m.group(1))
        if ms > 0 and sh_s == 0:
            sh_h, sh_s = mh, ms * 0.5
        if ms > 0 and hi_s == 0:
            hi_h, hi_s = mh, ms * 0.25

    sh_r, sh_g, sh_b = hsl_to_rgb_delta(sh_h, sh_s, max_gain=8.0)
    hi_r, hi_g, hi_b = hsl_to_rgb_delta(hi_h, hi_s, max_gain=6.0)

    # Grain
    grain_amt_m = re.search(r'crs:GrainAmount="(\d+)"', txt)
    grain_sz_m = re.search(r'crs:GrainSize="(\d+)"', txt)
    grain_amt = int(grain_amt_m.group(1)) if grain_amt_m else 0
    grain_sz = int(grain_sz_m.group(1)) if grain_sz_m else 10

    short_name = name.replace("Kodak_", "").replace("Fujifilm_", "FUJI_").replace("parvec", "").strip().upper()[:8]

    fs_content = f"""# {name} - converted from Lightroom PARVEC preset with skin tone calibration
VERSION   4
NAME      {short_name}
DESC      LIGHTROOM PROFILE WITH CALIBRATED SKIN TONES
WB        AS_SHOT
HSL       1
CURVE     {" ".join(curve_pts)}
CURVE_R   0,0 255,255
CURVE_G   0,0 255,255
CURVE_B   0,0 255,255
SAT       92
HUE_SAT   {",".join(str(x) for x in hue_sat)}
HUE_ROT   {",".join(str(x) for x in hue_rot)}
HUE_LUM   {",".join(str(x) for x in hue_lum)}
SPLIT_SH  {sh_r},{sh_g},{sh_b}
SPLIT_HI  {hi_r},{hi_g},{hi_b}
GRAIN     {min(grain_amt, 25)}
GRAIN_SIZE {min(grain_sz, 20)}
"""
    return fs_content

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: xmp2fs.py <file.xmp> [out.fs]")
        sys.exit(1)
    xmp_file = sys.argv[1]
    out_fs = sys.argv[2] if len(sys.argv) > 2 else sys.stdout
    res = convert_xmp(xmp_file)
    if isinstance(out_fs, str):
        with open(out_fs, 'w') as f:
            f.write(res)
        print(f"Wrote {out_fs}")
    else:
        print(res)
