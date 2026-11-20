#!/usr/bin/env python3
"""
Convert Adobe Lightroom / Camera Raw (.xmp) presets to Magic Lantern .fs recipes
(VERSION 3, 48-sector hue engine).

Supported Lightroom / ACR features:
  - Basic tone panel: Exposure2012, Contrast2012, Highlights2012, Shadows2012,
    Whites2012, Blacks2012, Saturation, Vibrance, Temperature/Tint offsets
  - Parametric tone curve: ParametricShadows, ParametricDarks, ParametricLights,
    ParametricHighlights (with Shadow/Midtone/Highlight splits)
  - Point tone curves: ToneCurvePV2012 (Master), ToneCurvePV2012Red,
    ToneCurvePV2012Green, ToneCurvePV2012Blue
  - 8-band non-uniform HSL: Red (0°), Orange (30°), Yellow (60°), Green (120°),
    Aqua (180°), Blue (240°), Purple (270°), Magenta (300°) -> interpolated
    onto Soup Engine's 48 uniform 7.5° sectors
  - PointColors (LR 13+): 19-float swathes blended onto the 48 hue sectors
  - Camera Calibration: ShadowTint, RedHue/Sat, GreenHue/Sat, BlueHue/Sat
  - 3-Way Color Grading / Split Toning: Shadow, Midtone (SPLIT_MID), Highlight
  - Grain: GrainAmount, GrainSize
  - Monochrome / B&W mix: ConvertToGrayscale + GrayMixer* bands
"""
import argparse
import math
import os
import re
import sys

BANDS = ["Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta"]
# Exact Lightroom band centers (matches se/engine/grade.c:115)
BAND_HUES = [0.0, 30.0, 60.0, 120.0, 180.0, 240.0, 270.0, 300.0, 360.0]
N_SECTORS = 48


def _get_float(txt: str, key: str, default: float = 0.0) -> float:
    m = re.search(rf'crs:{key}="([+-]?\d+(?:\.\d+)?)"', txt)
    if m:
        return float(m.group(1))
    m2 = re.search(rf'<crs:{key}>\s*([+-]?\d+(?:\.\d+)?)\s*</crs:{key}>', txt)
    return float(m2.group(1)) if m2 else default


def _get_str(txt: str, key: str, default: str = "") -> str:
    m = re.search(rf'crs:{key}="([^"]*)"', txt)
    return m.group(1).strip() if m else default


def _parse_curve_seq(txt: str, tag: str):
    m = re.search(rf'<crs:{tag}>\s*<rdf:Seq>(.*?)</rdf:Seq>\s*</crs:{tag}>', txt, re.DOTALL)
    if not m:
        return [(0, 0), (255, 255)]
    pts = []
    for li in re.findall(r'<rdf:li>\s*([+-]?\d+)\s*,\s*([+-]?\d+)\s*</rdf:li>', m.group(1)):
        x = max(0, min(255, int(li[0])))
        y = max(0, min(255, int(li[1])))
        pts.append((x, y))
    return pts if len(pts) >= 2 else [(0, 0), (255, 255)]


def _fritsch_carlson_eval(pts, x_val: float) -> float:
    """Monotone cubic Hermite spline matching se_curve_eval() in grade.c."""
    n = len(pts)
    if n < 2:
        return x_val
    xs = [p[0] / 255.0 for p in pts]
    ys = [p[1] / 255.0 for p in pts]
    x = x_val / 255.0
    if x <= xs[0]:
        return ys[0] * 255.0
    if x >= xs[-1]:
        return ys[-1] * 255.0

    d = []
    for i in range(n - 1):
        dx = xs[i + 1] - xs[i]
        d.append((ys[i + 1] - ys[i]) / dx if dx > 1e-6 else 0.0)

    m = [0.0] * n
    m[0] = d[0]
    m[-1] = d[-1]
    for i in range(1, n - 1):
        if d[i - 1] * d[i] <= 0.0:
            m[i] = 0.0
        else:
            m[i] = 0.5 * (d[i - 1] + d[i])

    for i in range(n - 1):
        if d[i] == 0.0:
            m[i] = 0.0
            m[i + 1] = 0.0
            continue
        a = m[i] / d[i]
        b = m[i + 1] / d[i]
        s = a * a + b * b
        if s > 9.0:
            f = 3.0 / math.sqrt(s)
            m[i] = f * a * d[i]
            m[i + 1] = f * b * d[i]

    k = 0
    for i in range(n - 1):
        if xs[i] <= x <= xs[i + 1]:
            k = i
            break
    h = xs[k + 1] - xs[k]
    t = (x - xs[k]) / h if h > 1e-6 else 0.0
    t2 = t * t
    t3 = t2 * t
    h00 = 2.0 * t3 - 3.0 * t2 + 1.0
    h10 = t3 - 2.0 * t2 + t
    h01 = -2.0 * t3 + 3.0 * t2
    h11 = t3 - t2
    y = h00 * ys[k] + h10 * h * m[k] + h01 * ys[k + 1] + h11 * h * m[k + 1]
    return max(0.0, min(255.0, y * 255.0))


def _apply_basic_and_parametric(v_norm: float, exposure: float, contrast: float,
                                highlights: float, shadows: float,
                                whites: float, blacks: float,
                                p_sh: float, p_dk: float, p_lt: float, p_hi: float,
                                s_split: float, m_split: float, h_split: float) -> float:
    """Evaluate Basic Panel + Parametric Curve on v in [0, 1] (matching grade.c)."""
    v = max(0.0, min(1.0, v_norm))
    if abs(exposure) > 1e-5:
        lin = v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4
        lin = max(0.0, min(1.0, lin * (2.0 ** exposure)))
        v = lin * 12.92 if lin <= 0.0031308 else 1.055 * (lin ** (1.0 / 2.4)) - 0.055

    if shadows != 0.0:
        w = 4.0 * v * ((1.0 - v) ** 3) * 1.7
        v += 0.42 * (shadows / 100.0) * w
    if highlights != 0.0:
        w = 4.0 * (v ** 3) * (1.0 - v) * 1.7
        v += 0.42 * (highlights / 100.0) * w
    if blacks != 0.0:
        w = (1.0 - v) ** 4
        v += 0.18 * (blacks / 100.0) * w
    if whites != 0.0:
        w = v ** 4
        v += 0.18 * (whites / 100.0) * w
    v = max(0.0, min(1.0, v))

    if contrast != 0.0:
        s = v * v * (3.0 - 2.0 * v)
        v = max(0.0, min(1.0, v + (s - v) * (contrast / 100.0)))

    # Parametric curve regions
    if any(abs(x) > 1e-5 for x in (p_sh, p_dk, p_lt, p_hi)):
        sp1 = max(0.05, min(0.95, s_split / 100.0))
        sp2 = max(sp1 + 0.05, min(0.95, m_split / 100.0))
        sp3 = max(sp2 + 0.05, min(0.95, h_split / 100.0))
        # Smooth bell weights pinned at 0 and 1
        pin = 4.0 * v * (1.0 - v)
        if p_sh != 0.0 and v < sp2:
            t = max(0.0, 1.0 - v / sp2)
            v += (p_sh / 100.0) * 0.18 * pin * (t * t)
        if p_dk != 0.0 and v < sp3:
            c = sp1
            w = math.exp(-((v - c) ** 2) / (2.0 * (0.18 ** 2)))
            v += (p_dk / 100.0) * 0.16 * pin * w
        if p_lt != 0.0 and v > sp1:
            c = sp3
            w = math.exp(-((v - c) ** 2) / (2.0 * (0.18 ** 2)))
            v += (p_lt / 100.0) * 0.16 * pin * w
        if p_hi != 0.0 and v > sp2:
            t = max(0.0, (v - sp2) / max(1e-4, 1.0 - sp2))
            v += (p_hi / 100.0) * 0.18 * pin * (t * t)
        v = max(0.0, min(1.0, v))

    return v


def _band_weights(hue_deg: float):
    w = [0.0] * 8
    h = hue_deg % 360.0
    for i in range(8):
        a, b = BAND_HUES[i], BAND_HUES[i + 1]
        if a <= h <= b:
            t = (h - a) / (b - a) if (b - a) > 1e-6 else 0.0
            w[i] += 1.0 - t
            w[(i + 1) % 8] += t
            return w
    return w


def _parse_point_colors(txt: str):
    """Parse Adobe CRS PointColors 19-float swathes."""
    m = re.search(r'<crs:PointColors>\s*<rdf:Seq>(.*?)</rdf:Seq>\s*</crs:PointColors>', txt, re.DOTALL)
    if not m:
        return []
    swathes = []
    for li in re.findall(r'<rdf:li>(.*?)</rdf:li>', m.group(1), re.DOTALL):
        vals = [float(x.strip()) for x in li.split(',') if x.strip()]
        if len(vals) >= 11 and vals[0] >= 0.0:
            src_hue_deg = (math.degrees(vals[0])) % 360.0
            hue_shift_deg = vals[3] * 30.0       # -1..+1 -> -30..+30 deg
            sat_shift_pct = vals[4] * 100.0      # -1..+1 -> -100..+100 %
            lum_shift_pct = vals[5] * 100.0      # -1..+1 -> -100..+100 %
            range_amt = vals[6] if len(vals) > 6 else 0.5
            # Effective half-width in degrees (default ~35 deg at range=0.5)
            half_w = max(12.0, 60.0 * max(0.15, range_amt))
            swathes.append((src_hue_deg, hue_shift_deg, sat_shift_pct, lum_shift_pct, half_w))
    return swathes


def hsl_to_rgb_delta(hue_deg: float, sat_pct: float, max_gain: float = 15.0):
    if sat_pct <= 0.0:
        return 0, 0, 0
    rad = math.radians(hue_deg)
    gain = (sat_pct / 100.0) * max_gain
    r = gain * math.cos(rad)
    g = gain * math.cos(rad - 2.0 * math.pi / 3.0)
    b = gain * math.cos(rad - 4.0 * math.pi / 3.0)
    mean = (r + g + b) / 3.0
    return int(round(r - mean)), int(round(g - mean)), int(round(b - mean))


def _format_curve_pts(pts) -> str:
    return " ".join(f"{int(round(x))},{int(round(y))}" for x, y in pts)


def convert_xmp(xmp_path: str, skin_warmth_fix: bool = False) -> str:
    with open(xmp_path, "r", encoding="utf-8", errors="ignore") as f:
        txt = f.read()

    name_m = re.search(r'<crs:Name>\s*<rdf:Alt>\s*<rdf:li[^>]*>(.*?)</rdf:li>', txt, re.DOTALL)
    if name_m and name_m.group(1).strip():
        name = name_m.group(1).strip()
    else:
        name = os.path.splitext(os.path.basename(xmp_path))[0].replace(" parvec", "")

    clean_name = re.sub(r'\s+parvec$', '', name, flags=re.IGNORECASE).replace("_", " ").strip()
    short_name = clean_name.upper()[:24]

    # 1. Basic Panel & Parametric Curve
    exposure = _get_float(txt, "Exposure2012", 0.0)
    contrast = _get_float(txt, "Contrast2012", 0.0)
    highlights = _get_float(txt, "Highlights2012", 0.0)
    shadows = _get_float(txt, "Shadows2012", 0.0)
    whites = _get_float(txt, "Whites2012", 0.0)
    blacks = _get_float(txt, "Blacks2012", 0.0)
    saturation = _get_float(txt, "Saturation", 0.0)
    vibrance = _get_float(txt, "Vibrance", 0.0)

    p_sh = _get_float(txt, "ParametricShadows", 0.0)
    p_dk = _get_float(txt, "ParametricDarks", 0.0)
    p_lt = _get_float(txt, "ParametricLights", 0.0)
    p_hi = _get_float(txt, "ParametricHighlights", 0.0)
    s_split = _get_float(txt, "ParametricShadowSplit", 25.0)
    m_split = _get_float(txt, "ParametricMidtoneSplit", 50.0)
    h_split = _get_float(txt, "ParametricHighlightSplit", 75.0)

    has_basic_tone = any(abs(x) > 1e-4 for x in (
        exposure, contrast, highlights, shadows, whites, blacks, p_sh, p_dk, p_lt, p_hi
    ))

    # 2. Point Curves (Master + R, G, B)
    pts_m = _parse_curve_seq(txt, "ToneCurvePV2012")
    pts_r = _parse_curve_seq(txt, "ToneCurvePV2012Red")
    pts_g = _parse_curve_seq(txt, "ToneCurvePV2012Green")
    pts_b = _parse_curve_seq(txt, "ToneCurvePV2012Blue")

    if has_basic_tone:
        knots = [0, 32, 64, 96, 128, 160, 192, 224, 255]
        baked_m = []
        for x in knots:
            v = _apply_basic_and_parametric(
                x / 255.0, exposure, contrast, highlights, shadows, whites, blacks,
                p_sh, p_dk, p_lt, p_hi, s_split, m_split, h_split
            )
            y = _fritsch_carlson_eval(pts_m, v * 255.0)
            baked_m.append((x, int(round(max(0.0, min(255.0, y))))))
        pts_m = baked_m

    # 3. 8-Band HSL + PointColors + Camera Calibration -> 48 Hue Sectors
    is_mono = _get_str(txt, "ConvertToGrayscale", "False").lower() == "true"
    hue_8, sat_8, lum_8 = [], [], []
    for b in BANDS:
        hue_8.append(_get_float(txt, f"HueAdjustment{b}", 0.0))
        sat_8.append(_get_float(txt, f"SaturationAdjustment{b}", 0.0))
        if is_mono:
            lum_8.append(_get_float(txt, f"GrayMixer{b}", 0.0))
        else:
            lum_8.append(_get_float(txt, f"LuminanceAdjustment{b}", 0.0))

    if skin_warmth_fix and not is_mono:
        if hue_8[1] > -13.0:
            hue_8[1] = -20.0
        if hue_8[2] > -6.0:
            hue_8[2] = -6.0
        if sat_8[2] > -20.0:
            sat_8[2] = -22.0

    point_colors = _parse_point_colors(txt)

    # Camera Calibration primaries (Red=0°, Green=120°, Blue=240°)
    cal_primaries = [
        (0.0,   _get_float(txt, "RedHue", 0.0) * 0.25,   _get_float(txt, "RedSaturation", 0.0) * 0.5),
        (120.0, _get_float(txt, "GreenHue", 0.0) * 0.25, _get_float(txt, "GreenSaturation", 0.0) * 0.5),
        (240.0, _get_float(txt, "BlueHue", 0.0) * 0.25,  _get_float(txt, "BlueSaturation", 0.0) * 0.5),
    ]

    sec_rot = []
    sec_sat = []
    sec_lum = []
    for k in range(N_SECTORS):
        deg = k * (360.0 / N_SECTORS)
        w = _band_weights(deg)
        dh = sum(w[i] * hue_8[i] for i in range(8)) * 0.30   # +/-100 -> +/-30 deg
        ds = sum(w[i] * sat_8[i] for i in range(8))
        dl = sum(w[i] * lum_8[i] for i in range(8))

        # Blend PointColors swathes
        for sh_deg, d_rot, d_sat, d_lum, half_w in point_colors:
            dist = abs(((deg - sh_deg + 180.0) % 360.0) - 180.0)
            if dist < half_w:
                pw = 0.5 * (1.0 + math.cos(math.pi * dist / half_w))
                dh += pw * d_rot
                ds += pw * d_sat
                dl += pw * d_lum

        # Blend Camera Calibration primaries (cosine lobe width 60 deg)
        for p_deg, p_rot, p_sat in cal_primaries:
            if abs(p_rot) > 1e-4 or abs(p_sat) > 1e-4:
                dist = abs(((deg - p_deg + 180.0) % 360.0) - 180.0)
                if dist < 60.0:
                    cw = 0.5 * (1.0 + math.cos(math.pi * dist / 60.0))
                    dh += cw * p_rot
                    ds += cw * p_sat

        sec_rot.append(round(max(-60.0, min(60.0, dh)), 1))
        sec_sat.append(0 if is_mono else int(round(max(0.0, min(250.0, 100.0 + ds)))))
        sec_lum.append(int(round(max(20.0, min(200.0, 100.0 + dl)))))

    # 4. 3-Way Split Toning / Color Grading (Shadow, Midtone, Highlight)
    sh_h = _get_float(txt, "SplitToningShadowHue", 0.0)
    sh_s = _get_float(txt, "SplitToningShadowSaturation", 0.0)
    mid_h = _get_float(txt, "ColorGradeMidtoneHue", 0.0)
    mid_s = _get_float(txt, "ColorGradeMidtoneSat", 0.0)
    hi_h = _get_float(txt, "SplitToningHighlightHue", 0.0)
    hi_s = _get_float(txt, "SplitToningHighlightSaturation", 0.0)
    shadow_tint = _get_float(txt, "ShadowTint", 0.0)

    sh_r, sh_g, sh_b = hsl_to_rgb_delta(sh_h, sh_s, max_gain=8.0)
    if abs(shadow_tint) > 1e-4:
        # ShadowTint: negative = green, positive = magenta
        tint_shift = int(round(shadow_tint * 0.08))
        sh_r += tint_shift
        sh_g -= tint_shift
        sh_b += tint_shift
    mid_r, mid_g, mid_b = hsl_to_rgb_delta(mid_h, mid_s, max_gain=6.0)
    hi_r, hi_g, hi_b = hsl_to_rgb_delta(hi_h, hi_s, max_gain=6.0)

    # 5. Global Saturation & Grain
    global_sat = 0 if is_mono else int(round(max(0.0, min(200.0, 100.0 + saturation + vibrance * 0.6))))
    grain_amt = int(round(_get_float(txt, "GrainAmount", 0.0)))
    grain_sz = int(round(_get_float(txt, "GrainSize", 10.0)))

    lines = [
        f"# {clean_name} - converted from Lightroom XMP ({os.path.basename(xmp_path)})",
        "VERSION 3",
        f"NAME {short_name}",
        f"DESC LIGHTROOM XMP PRESET ({clean_name.upper()[:28]})",
        "WB AS_SHOT",
        f"CURVE {_format_curve_pts(pts_m)}",
        f"CURVE_R {_format_curve_pts(pts_r)}",
        f"CURVE_G {_format_curve_pts(pts_g)}",
        f"CURVE_B {_format_curve_pts(pts_b)}",
        f"SAT {global_sat}",
        f"HUE_SAT {','.join(str(x) for x in sec_sat)}",
        f"HUE_ROT {','.join(str(x) for x in sec_rot)}",
        f"HUE_LUM {','.join(str(x) for x in sec_lum)}",
        f"SPLIT_SH {sh_r},{sh_g},{sh_b}",
        f"SPLIT_MID {mid_r},{mid_g},{mid_b}",
        f"SPLIT_HI {hi_r},{hi_g},{hi_b}",
        f"GRAIN {min(grain_amt, 40)}",
        f"GRAIN_SIZE {min(max(grain_sz, 1), 50)}",
        "",
    ]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description="Convert Adobe Lightroom .xmp preset to Magic Lantern .fs (48-sector)")
    ap.add_argument("xmp", help="Input .xmp file")
    ap.add_argument("out", nargs="?", help="Output .fs file (prints to stdout if omitted)")
    ap.add_argument("--skin-fix", action="store_true", help="Apply optional yellow-cast skin warmth clamp")
    args = ap.parse_args()

    res = convert_xmp(args.xmp, skin_warmth_fix=args.skin_fix)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(res)
        print(f"Wrote {args.out} ({len(res)} bytes)")
    else:
        sys.stdout.write(res)


if __name__ == "__main__":
    main()
