#!/usr/bin/env python3
"""
Film Simulation Comparison and Color Fidelity Evaluation Suite
=============================================================
Compares Magic Lantern Film Sim Engine renders against reference targets
(e.g., Dehancer Desktop, Lightroom profiles, ColorChecker charts) across
critical photographic color zones, computing Delta E (2000 and 76),
hue shift, chroma ratio, channel balance, and visual comparison assets.

DO NOT RUN AUTOMATICALLY - Awaiting user review of evaluation plan.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import numpy as np
from PIL import Image, ImageDraw, ImageFont

# ==============================================================================
# COLOR SPACE TRANSFORMATIONS (SRGB <-> LAB <-> LCH <-> HSV)
# ==============================================================================

def srgb_to_linear(rgb: np.ndarray) -> np.ndarray:
    """Converts sRGB [0..1] to linear RGB."""
    a = 0.055
    return np.where(rgb <= 0.04045, rgb / 12.92, ((rgb + a) / (1.0 + a)) ** 2.4)

def linear_to_xyz(rgb_lin: np.ndarray) -> np.ndarray:
    """Converts linear RGB (D65) to CIE XYZ (D65)."""
    # Matrix for sRGB D65
    M = np.array([
        [0.4124564, 0.3575761, 0.1804375],
        [0.2126729, 0.7151522, 0.0721750],
        [0.0193339, 0.1191920, 0.9503041]
    ])
    return rgb_lin @ M.T

def xyz_to_lab(xyz: np.ndarray) -> np.ndarray:
    """Converts CIE XYZ (D65 normalized to [0..1]) to CIE L*a*b*."""
    # D65 reference white point
    Xn, Yn, Zn = 0.95047, 1.00000, 1.08883
    
    x = xyz[..., 0] / Xn
    y = xyz[..., 1] / Yn
    z = xyz[..., 2] / Zn
    
    delta = 6.0 / 29.0
    delta_cube = delta ** 3
    factor = 1.0 / (3.0 * delta ** 2)
    
    fx = np.where(x > delta_cube, np.cbrt(x), factor * x + 4.0 / 29.0)
    fy = np.where(y > delta_cube, np.cbrt(y), factor * y + 4.0 / 29.0)
    fz = np.where(z > delta_cube, np.cbrt(z), factor * z + 4.0 / 29.0)
    
    L = 116.0 * fy - 16.0
    a = 500.0 * (fx - fy)
    b = 200.0 * (fy - fz)
    
    return np.stack([L, a, b], axis=-1)

def srgb_to_lab(srgb: np.ndarray) -> np.ndarray:
    """Converts sRGB array [0..255] or [0..1] to CIE L*a*b*."""
    arr = srgb.astype(np.float64)
    if arr.max() > 1.0:
        arr /= 255.0
    lin = srgb_to_linear(arr)
    xyz = linear_to_xyz(lin)
    return xyz_to_lab(xyz)

def lab_to_lch(lab: np.ndarray) -> np.ndarray:
    """Converts CIE L*a*b* to L*C*h (h in degrees [0..360))."""
    L = lab[..., 0]
    a = lab[..., 1]
    b = lab[..., 2]
    C = np.hypot(a, b)
    h = np.degrees(np.arctan2(b, a)) % 360.0
    return np.stack([L, C, h], axis=-1)

def rgb_to_hsv(rgb: np.ndarray) -> np.ndarray:
    """Converts sRGB [0..255] to HSV (H in [0..360), S in [0..1], V in [0..1])."""
    arr = rgb.astype(np.float64)
    if arr.max() > 1.0:
        arr /= 255.0
    r, g, b = arr[..., 0], arr[..., 1], arr[..., 2]
    
    cmax = np.maximum(np.maximum(r, g), b)
    cmin = np.minimum(np.minimum(r, g), b)
    delta = cmax - cmin
    
    # Hue
    h = np.zeros_like(cmax)
    mask_r = (cmax == r) & (delta > 1e-6)
    mask_g = (cmax == g) & (delta > 1e-6)
    mask_b = (cmax == b) & (delta > 1e-6)
    
    h[mask_r] = ((g[mask_r] - b[mask_r]) / delta[mask_r]) % 6.0
    h[mask_g] = ((b[mask_g] - r[mask_g]) / delta[mask_g]) + 2.0
    h[mask_b] = ((r[mask_b] - g[mask_b]) / delta[mask_b]) + 4.0
    h = (h * 60.0) % 360.0
    
    # Saturation
    s = np.where(cmax > 1e-6, delta / cmax, 0.0)
    v = cmax
    
    return np.stack([h, s, v], axis=-1)

# ==============================================================================
# CIEDE2000 COLOR DIFFERENCE (INDUSTRY GOLD STANDARD)
# ==============================================================================

def ciede2000(lab1: np.ndarray, lab2: np.ndarray, kL: float = 1.0, kC: float = 1.0, kH: float = 1.0) -> np.ndarray:
    """
    Computes CIEDE2000 color difference between two arrays of CIE L*a*b* points.
    Matches standard ISO/CIE 11664-6 formulation.
    """
    L1, a1, b1 = lab1[..., 0], lab1[..., 1], lab1[..., 2]
    L2, a2, b2 = lab2[..., 0], lab2[..., 1], lab2[..., 2]
    
    C1 = np.hypot(a1, b1)
    C2 = np.hypot(a2, b2)
    C_bar = 0.5 * (C1 + C2)
    
    G = 0.5 * (1.0 - np.sqrt((C_bar ** 7) / (C_bar ** 7 + 25.0 ** 7 + 1e-12)))
    
    a1_p = (1.0 + G) * a1
    a2_p = (1.0 + G) * a2
    
    C1_p = np.hypot(a1_p, b1)
    C2_p = np.hypot(a2_p, b2)
    
    h1_p = np.degrees(np.arctan2(b1, a1_p)) % 360.0
    h2_p = np.degrees(np.arctan2(b2, a2_p)) % 360.0
    
    delta_L_p = L2 - L1
    delta_C_p = C2_p - C1_p
    
    # Delta h' computation
    delta_h_p = np.zeros_like(h1_p)
    mask_c = (C1_p * C2_p) > 1e-12
    dh = h2_p - h1_p
    
    dh_cond1 = mask_c & (np.abs(dh) <= 180.0)
    dh_cond2 = mask_c & (dh > 180.0)
    dh_cond3 = mask_c & (dh < -180.0)
    
    delta_h_p[dh_cond1] = dh[dh_cond1]
    delta_h_p[dh_cond2] = dh[dh_cond2] - 360.0
    delta_h_p[dh_cond3] = dh[dh_cond3] + 360.0
    
    delta_H_p = 2.0 * np.sqrt(C1_p * C2_p + 1e-12) * np.sin(np.radians(delta_h_p / 2.0))
    
    L_bar_p = 0.5 * (L1 + L2)
    C_bar_p = 0.5 * (C1_p + C2_p)
    
    # Average h' computation
    h_bar_p = np.zeros_like(h1_p)
    h_sum = h1_p + h2_p
    diff_abs = np.abs(h1_p - h2_p)
    
    h_cond1 = mask_c & (diff_abs <= 180.0)
    h_cond2 = mask_c & (diff_abs > 180.0) & (h_sum < 360.0)
    h_cond3 = mask_c & (diff_abs > 180.0) & (h_sum >= 360.0)
    
    h_bar_p[~mask_c] = h_sum[~mask_c]
    h_bar_p[h_cond1] = 0.5 * h_sum[h_cond1]
    h_bar_p[h_cond2] = 0.5 * (h_sum[h_cond2] + 360.0)
    h_bar_p[h_cond3] = 0.5 * (h_sum[h_cond3] - 360.0)
    
    T = (1.0 - 0.17 * np.cos(np.radians(h_bar_p - 30.0)) +
         0.24 * np.cos(np.radians(2.0 * h_bar_p)) +
         0.32 * np.cos(np.radians(3.0 * h_bar_p + 6.0)) -
         0.20 * np.cos(np.radians(4.0 * h_bar_p - 63.0)))
    
    delta_theta = 30.0 * np.exp(-(((h_bar_p - 275.0) / 25.0) ** 2))
    R_C = 2.0 * np.sqrt((C_bar_p ** 7) / (C_bar_p ** 7 + 25.0 ** 7 + 1e-12))
    S_L = 1.0 + (0.015 * ((L_bar_p - 50.0) ** 2)) / np.sqrt(20.0 + ((L_bar_p - 50.0) ** 2))
    S_C = 1.0 + 0.045 * C_bar_p
    S_H = 1.0 + 0.015 * C_bar_p * T
    R_T = -np.sin(np.radians(2.0 * delta_theta)) * R_C
    
    term_L = delta_L_p / (kL * S_L)
    term_C = delta_C_p / (kC * S_C)
    term_H = delta_H_p / (kH * S_H)
    
    de00 = np.sqrt(term_L ** 2 + term_C ** 2 + term_H ** 2 + R_T * term_C * term_H)
    return de00

# ==============================================================================
# COLOR ZONE DEFINITIONS & PHOTOGRAPHIC CRITICAL REGIONS
# ==============================================================================

@dataclass
class ColorZone:
    name: str
    description: str
    # HSV bounding criteria in source/ref
    hue_range: Tuple[float, float]  # in [0..360)
    sat_range: Tuple[float, float]  # in [0..1]
    val_range: Tuple[float, float]  # in [0..1]
    # Human vision tolerance threshold (Delta E 2000)
    target_max_de00: float
    target_mean_de00: float

COLOR_ZONES: List[ColorZone] = [
    # --------------------------------------------------------------------------
    # SPECIALIZED SKIN TONE ZONES (Fine-grained Fitzpatrick & Monk Scale)
    # --------------------------------------------------------------------------
    ColorZone(
        name="skin_fair_highlight",
        description="Type I/II Fair & Specular Skin Highlights (forehead, nose bridge, cheekbones). Primary guard against yellow highlight dumping and green curve shoulder dominance.",
        hue_range=(20.0, 46.0),
        sat_range=(0.08, 0.35),
        val_range=(0.78, 0.98),
        target_max_de00=2.8,
        target_mean_de00=1.4,
    ),
    ColorZone(
        name="skin_peach_midtone",
        description="Type II/III Caucasian & Warm Peach Midtones (healthy cheek and skin core). Demands positive R-G differential (+18..+32) and hue 24°-32°.",
        hue_range=(20.0, 36.0),
        sat_range=(0.18, 0.58),
        val_range=(0.50, 0.82),
        target_max_de00=3.0,
        target_mean_de00=1.6,
    ),
    ColorZone(
        name="skin_asian_golden",
        description="Type III/IV East Asian & Warm Golden Undertones (subtle yellow-gold warmth). Must maintain organic peach base without tipping into greenish-yellow (h > 40°).",
        hue_range=(30.0, 44.0),
        sat_range=(0.16, 0.55),
        val_range=(0.48, 0.80),
        target_max_de00=3.2,
        target_mean_de00=1.7,
    ),
    ColorZone(
        name="skin_olive_undertone",
        description="Type IV Mediterranean, Latin & South Asian Olive Skin. High sensitivity to green shifts; requires strict R-G > 12 to avoid sickly/muddy appearance.",
        hue_range=(26.0, 42.0),
        sat_range=(0.14, 0.45),
        val_range=(0.40, 0.72),
        target_max_de00=3.2,
        target_mean_de00=1.8,
    ),
    ColorZone(
        name="skin_deep_melanin",
        description="Type V/VI Deep Bronze, Brown & Dark African Skin Tones. High saturation and warm caramel/chocolate shadow retention; tests D-min toe lift and deep skin contrast.",
        hue_range=(14.0, 34.0),
        sat_range=(0.20, 0.65),
        val_range=(0.20, 0.52),
        target_max_de00=3.5,
        target_mean_de00=1.9,
    ),
    ColorZone(
        name="skin_blush_and_lips",
        description="Cheek Blush, Natural Lip Tones & Backlit Ear Subsurface Glow. Transition between Rose (Wedge 11) and Red (Wedge 0).",
        hue_range=(345.0, 18.0),  # Wraps around 0° (Magenta-Rose to Red-Orange)
        sat_range=(0.22, 0.75),
        val_range=(0.35, 0.85),
        target_max_de00=3.8,
        target_mean_de00=2.0,
    ),
    ColorZone(
        name="skin_shadow_contours",
        description="Under-chin, eye sockets, neck and ambient skin shadows. Tests against muddy ash-grey desaturation or dirty purple split-shadow leaks.",
        hue_range=(12.0, 38.0),
        sat_range=(0.14, 0.50),
        val_range=(0.16, 0.42),
        target_max_de00=3.8,
        target_mean_de00=2.0,
    ),

    # --------------------------------------------------------------------------
    # ENVIRONMENTAL & REFERENCE ZONES
    # --------------------------------------------------------------------------
    ColorZone(
        name="sky_deep_blue",
        description="Open zenith blue sky and clear atmospheric gradients",
        hue_range=(205.0, 235.0),
        sat_range=(0.20, 0.85),
        val_range=(0.45, 0.95),
        target_max_de00=4.0,
        target_mean_de00=2.0,
    ),
    ColorZone(
        name="sky_cyan_haze",
        description="Horizon haze, sea foam, and atmospheric aerial perspective cyan",
        hue_range=(180.0, 208.0),
        sat_range=(0.12, 0.60),
        val_range=(0.50, 0.95),
        target_max_de00=3.8,
        target_mean_de00=1.9,
    ),
    ColorZone(
        name="foliage_lush_green",
        description="Summer leaves, grass, and forest canopy (checking chlorophyll green response)",
        hue_range=(75.0, 145.0),
        sat_range=(0.18, 0.75),
        val_range=(0.20, 0.78),
        target_max_de00=4.5,
        target_mean_de00=2.2,
    ),
    ColorZone(
        name="foliage_olive_warm",
        description="Dry grass, moss, pine needles, and warm earthy greens",
        hue_range=(55.0, 80.0),
        sat_range=(0.18, 0.65),
        val_range=(0.20, 0.70),
        target_max_de00=3.8,
        target_mean_de00=2.0,
    ),
    ColorZone(
        name="warm_golden_hour",
        description="Sunset accents, terracotta, autumn leaves, and warm incandescent light",
        hue_range=(25.0, 60.0),
        sat_range=(0.45, 0.95),
        val_range=(0.45, 0.98),
        target_max_de00=3.5,
        target_mean_de00=1.8,
    ),
    ColorZone(
        name="specular_highlights",
        description="Neutral bright highlights (Y > 230): tests SPLIT_HI bleed, curve convergence, white point",
        hue_range=(0.0, 360.0),
        sat_range=(0.00, 0.10),
        val_range=(0.90, 1.00),
        target_max_de00=2.5,
        target_mean_de00=1.2,
    ),
    ColorZone(
        name="deep_shadow_base",
        description="Film base D-min shadows (Y < 30): tests SPLIT_SH shadow tint and toe lift/crush",
        hue_range=(0.0, 360.0),
        sat_range=(0.00, 0.35),
        val_range=(0.00, 0.15),
        target_max_de00=3.0,
        target_mean_de00=1.5,
    ),
]

# Standard Macbeth 24 Patch Reference Coordinates (sRGB D65 nominal)
MACBETH_24_NAMES = [
    "Dark Skin", "Light Skin", "Blue Sky", "Foliage", "Blue Flower", "Bluish Green",
    "Orange", "Purplish Blue", "Moderate Red", "Purple", "Yellow Green", "Orange Yellow",
    "Blue", "Green", "Red", "Yellow", "Magenta", "Cyan",
    "White 9.5", "Neutral 8", "Neutral 6.5", "Neutral 5", "Neutral 3.5", "Black 2"
]

# ==============================================================================
# SPECIALIZED 24-PATCH SKIN TONE CALIBRATION CHART (FITZPATRICK & MONK SCALES)
# ==============================================================================

@dataclass
class SkinPatch:
    index: int
    name: str
    category: str
    nominal_rgb: Tuple[int, int, int]
    note: str

SKIN_CHART_24: List[SkinPatch] = [
    # Row 1: Fair / Highlights & Specular Roll-off (Type I - II)
    SkinPatch(0,  "Porcelain Light",      "Fair Highlights", (255, 230, 218), "Type I cool undertone, tests red-cyan channel balance"),
    SkinPatch(1,  "Ivory Sheen",          "Fair Highlights", (250, 218, 198), "Type I/II neutral highlight, detects yellow cast"),
    SkinPatch(2,  "Warm Forehead Highlight","Fair Highlights",(244, 206, 178), "Type II sunlit skin, detects green curve shoulder"),
    SkinPatch(3,  "Golden Specular",      "Fair Highlights", (242, 198, 162), "Specular roll-off, tests SPLIT_HI yellow push"),
    SkinPatch(4,  "Cheekbone Glint",      "Fair Highlights", (255, 238, 226), "Near-specular L*=93, checks highlight desaturation"),
    SkinPatch(5,  "Fair Blush",           "Fair Highlights", (245, 192, 185), "Rose-Red skin flush, checks Wedge 11/0 blend"),

    # Row 2: Caucasian & East Asian Midtones (Type II - IV)
    SkinPatch(6,  "Classic Peach Midtone","Core Midtones",   (232, 175, 142), "Standard Portra peach baseline (h=28°, R-G=+25)"),
    SkinPatch(7,  "Warm Beige",           "Core Midtones",   (222, 162, 126), "Type III warm Caucasian, tests saturation retention"),
    SkinPatch(8,  "East Asian Warm",      "Core Midtones",   (226, 174, 128), "Type III East Asian golden undertone (h=35°)"),
    SkinPatch(9,  "East Asian Fair",      "Core Midtones",   (238, 188, 154), "Type II/III neutral East Asian cheek"),
    SkinPatch(10, "Natural Lip / Flush",  "Core Midtones",   (210, 128, 122), "Subsurface blood glow, validates Wedge 0 (Red)"),
    SkinPatch(11, "Cheek Peach Glow",     "Core Midtones",   (240, 168, 130), "High-chroma portrait skin accent"),

    # Row 3: Olive, Mediterranean & South Asian (Type IV - V)
    SkinPatch(12, "Olive Tone Light",     "Olive / Tan",     (206, 155, 118), "Type IV Mediterranean neutral (tests R-G > +15)"),
    SkinPatch(13, "Olive Tone Deep",      "Olive / Tan",     (184, 132,  96), "Type IV olive undertone (prevents greenish cast)"),
    SkinPatch(14, "Latin Warm Tan",       "Olive / Tan",     (198, 140, 102), "Sun-kissed Latin/Hispanic midtone"),
    SkinPatch(15, "South Asian Honey",    "Olive / Tan",     (188, 128,  86), "Type V warm South Asian golden brown"),
    SkinPatch(16, "Warm Bronze",          "Olive / Tan",     (172, 114,  76), "Bronze skin contour, checks mid-shadow roll-off"),
    SkinPatch(17, "Amber Ochre Tan",      "Olive / Tan",     (194, 134,  84), "Golden hour skin reflection"),

    # Row 4: Deep Melanin & Shadow Contours (Type V - VI)
    SkinPatch(18, "Rich Caramel",         "Deep Melanin",    (152,  96,  62), "Type V rich caramel brown, tests saturation decay"),
    SkinPatch(19, "Dark Amber Brown",     "Deep Melanin",    (128,  78,  48), "Type VI African deep brown, checks D-min lift"),
    SkinPatch(20, "Espresso Warm",        "Deep Melanin",    (102,  60,  38), "Type VI deep espresso, checks warm toe curl"),
    SkinPatch(21, "Mahogany Deep",        "Deep Melanin",    ( 82,  48,  32), "Deep melanin highlight reflection"),
    SkinPatch(22, "Skin Jawline Shadow",  "Skin Shadows",    (118,  76,  58), "Ambient skin shadow (prevents dirty ash-grey)"),
    SkinPatch(23, "Skin Under-Eye Shadow","Skin Shadows",    (138,  88,  74), "Subsurface shadow contour (prevents muddy purple)")
]

def generate_skin_calibration_chart(output_path: Path, patch_size: int = 120, border: int = 12) -> Image.Image:
    """
    Renders a standalone 24-patch Skin Tone Calibration Chart (4 rows x 6 cols)
    with reference metadata and gray surrounds for grading evaluation.
    """
    cols = 6
    rows = 4
    w = cols * patch_size + (cols + 1) * border
    h = rows * patch_size + (rows + 1) * border + 40  # 40px bottom bar for neutral step ramp
    
    # 18% neutral grey surround (sRGB 118, 118, 118) to avoid optical contrast illusions
    chart_img = Image.new("RGB", (w, h), (118, 118, 118))
    draw = ImageDraw.Draw(chart_img)
    
    for idx, patch in enumerate(SKIN_CHART_24):
        col = idx % cols
        row = idx // cols
        x0 = border + col * (patch_size + border)
        y0 = border + row * (patch_size + border)
        x1 = x0 + patch_size
        y1 = y0 + patch_size
        draw.rectangle([x0, y0, x1, y1], fill=patch.nominal_rgb)
        
    # Neutral reference strip at bottom (0, 64, 128, 192, 255)
    bar_y0 = h - 32
    bar_y1 = h - 8
    n_steps = 6
    step_w = (w - 2 * border) // n_steps
    for i in range(n_steps):
        val = int(i * 255 / (n_steps - 1))
        bx0 = border + i * step_w
        bx1 = bx0 + step_w
        draw.rectangle([bx0, bar_y0, bx1, bar_y1], fill=(val, val, val))
        
    output_path.parent.mkdir(parents=True, exist_ok=True)
    chart_img.save(str(output_path), quality=98)
    return chart_img

# ==============================================================================
# ZONE METRIC EVALUATOR
# ==============================================================================

@dataclass
class ZoneEvaluationResult:
    zone_name: str
    pixel_count: int
    mean_de00: float
    p95_de00: float
    max_de00: float
    mean_de76: float
    
    # Hue metrics
    ref_hue_deg: float
    engine_hue_deg: float
    delta_hue_deg: float
    
    # Chroma metrics
    ref_chroma: float
    engine_chroma: float
    chroma_ratio: float
    
    # Channel differentials (crucial for skin diagnostic)
    ref_rg_diff: float      # R - G
    engine_rg_diff: float
    ref_rb_diff: float      # R - B
    engine_rb_diff: float
    
    # Luminance metrics
    ref_L: float
    engine_L: float
    delta_L: float
    
    passed_tolerance: bool
    diagnostic: str

def evaluate_zone(
    ref_rgb: np.ndarray,
    eng_rgb: np.ndarray,
    mask: np.ndarray,
    zone: ColorZone
) -> Optional[ZoneEvaluationResult]:
    """Computes comprehensive color fidelity metrics over a masked zone."""
    n_pixels = int(np.sum(mask))
    if n_pixels < 50:
        return None
        
    ref_pixels = ref_rgb[mask].astype(np.float64)
    eng_pixels = eng_rgb[mask].astype(np.float64)
    
    ref_lab = srgb_to_lab(ref_pixels)
    eng_lab = srgb_to_lab(eng_pixels)
    
    de00 = ciede2000(eng_lab, ref_lab)
    de76 = np.linalg.norm(eng_lab - ref_lab, axis=-1)
    
    ref_lch = lab_to_lch(ref_lab)
    eng_lch = lab_to_lch(eng_lab)
    
    # Mean L, C, h
    ref_L = float(np.mean(ref_lch[:, 0]))
    eng_L = float(np.mean(eng_lch[:, 0]))
    ref_C = float(np.mean(ref_lch[:, 1]))
    eng_C = float(np.mean(eng_lch[:, 1]))
    
    # Circular mean for hue
    ref_h_rad = np.radians(ref_lch[:, 2])
    eng_h_rad = np.radians(eng_lch[:, 2])
    ref_h = float(np.degrees(np.arctan2(np.mean(np.sin(ref_h_rad)), np.mean(np.cos(ref_h_rad))))) % 360.0
    eng_h = float(np.degrees(np.arctan2(np.mean(np.sin(eng_h_rad)), np.mean(np.cos(eng_h_rad))))) % 360.0
    
    dh = eng_h - ref_h
    if dh > 180.0:
        dh -= 360.0
    elif dh < -180.0:
        dh += 360.0
        
    chroma_ratio = (eng_C / ref_C) if ref_C > 1e-4 else 1.0
    
    # R-G, R-B differentials
    ref_rg = float(np.mean(ref_pixels[:, 0] - ref_pixels[:, 1]))
    eng_rg = float(np.mean(eng_pixels[:, 0] - eng_pixels[:, 1]))
    ref_rb = float(np.mean(ref_pixels[:, 0] - ref_pixels[:, 2]))
    eng_rb = float(np.mean(eng_pixels[:, 0] - eng_pixels[:, 2]))
    
    mean_de00 = float(np.mean(de00))
    p95_de00 = float(np.percentile(de00, 95))
    max_de00 = float(np.max(de00))
    
    passed = (mean_de00 <= zone.target_mean_de00) and (p95_de00 <= zone.target_max_de00 * 1.25)
    
    # Formulate diagnostic text
    notes = []
    if dh > 4.0:
        notes.append(f"Hue rotated +{dh:.1f}° toward yellow/green")
    elif dh < -4.0:
        notes.append(f"Hue rotated {dh:.1f}° toward magenta/red")
        
    if chroma_ratio < 0.85:
        notes.append(f"Undersaturated ({(1.0 - chroma_ratio)*100:.1f}% below ref)")
    elif chroma_ratio > 1.18:
        notes.append(f"Oversaturated ({(chroma_ratio - 1.0)*100:.1f}% above ref)")
        
    if abs(eng_L - ref_L) > 4.0:
        notes.append(f"Luma offset (ΔL* = {eng_L - ref_L:+.1f})")
        
    diagnostic = "; ".join(notes) if notes else "Well matched"
    
    return ZoneEvaluationResult(
        zone_name=zone.name,
        pixel_count=n_pixels,
        mean_de00=mean_de00,
        p95_de00=p95_de00,
        max_de00=max_de00,
        mean_de76=float(np.mean(de76)),
        ref_hue_deg=ref_h,
        engine_hue_deg=eng_h,
        delta_hue_deg=dh,
        ref_chroma=ref_C,
        engine_chroma=eng_C,
        chroma_ratio=chroma_ratio,
        ref_rg_diff=ref_rg,
        engine_rg_diff=eng_rg,
        ref_rb_diff=ref_rb,
        engine_rb_diff=eng_rb,
        ref_L=ref_L,
        engine_L=eng_L,
        delta_L=eng_L - ref_L,
        passed_tolerance=passed,
        diagnostic=diagnostic,
    )

# ==============================================================================
# ZONE MASK GENERATOR
# ==============================================================================

def create_zone_mask(rgb_img: np.ndarray, zone: ColorZone) -> np.ndarray:
    """Creates a boolean pixel mask corresponding to a ColorZone's criteria."""
    hsv = rgb_to_hsv(rgb_img)
    h, s, v = hsv[..., 0], hsv[..., 1], hsv[..., 2]
    
    h_min, h_max = zone.hue_range
    if h_min <= h_max:
        h_mask = (h >= h_min) & (h <= h_max)
    else:  # Wraps around 360 (e.g. magenta-red [340..20])
        h_mask = (h >= h_min) | (h <= h_max)
        
    s_mask = (s >= zone.sat_range[0]) & (s <= zone.sat_range[1])
    v_mask = (v >= zone.val_range[0]) & (v <= zone.val_range[1])
    
    return h_mask & s_mask & v_mask

# ==============================================================================
# VISUAL ASSET RENDERERS & SIDE-BY-SIDE BUILDERS
# ==============================================================================

def create_differential_heatmap(ref_rgb: np.ndarray, eng_rgb: np.ndarray) -> Image.Image:
    """Generates an intuitive false-color Delta E 2000 heatmap."""
    ref_lab = srgb_to_lab(ref_rgb)
    eng_lab = srgb_to_lab(eng_rgb)
    de00 = ciede2000(eng_lab, ref_lab)
    
    # Colormap: 0 = Dark Navy, 2 = Cyan, 4 = Green, 7 = Yellow, 10+ = Fiery Red/White
    norm_de = np.clip(de00 / 10.0, 0.0, 1.0)
    
    # 3-channel false color mapping
    r = np.clip(2.0 * norm_de - 0.5, 0.0, 1.0)
    g = np.clip(1.0 - 2.0 * np.abs(norm_de - 0.5), 0.0, 1.0)
    b = np.clip(1.0 - 2.0 * norm_de, 0.0, 1.0)
    
    heatmap_rgb = (np.stack([r, g, b], axis=-1) * 255.0).astype(np.uint8)
    return Image.fromarray(heatmap_rgb)

def build_comparison_sheet(
    engine_img: Image.Image,
    ref_img: Image.Image,
    stock_name: str,
    frame_name: str,
    results: List[ZoneEvaluationResult],
    heatmap: Optional[Image.Image] = None
) -> Image.Image:
    """Builds a formatted visual review board with labeled headers and metrics."""
    w, h = engine_img.size
    banner_h = 70
    footer_h = 110 if heatmap is None else 110
    
    total_w = w * 2 if heatmap is None else w * 3
    total_h = h + banner_h + footer_h
    
    canvas = Image.new("RGB", (total_w, total_h), (24, 25, 28))
    draw = ImageDraw.Draw(canvas)
    
    # Paste Images
    canvas.paste(engine_img, (0, banner_h))
    canvas.paste(ref_img, (w, banner_h))
    if heatmap is not None:
        canvas.paste(heatmap, (w * 2, banner_h))
        
    # Top Banner
    draw.rectangle([0, 0, total_w, banner_h], fill=(18, 19, 21))
    draw.text((20, 16), f"FILM SIM EVALUATION: {stock_name.upper()} — Frame {frame_name}", fill=(240, 240, 240))
    draw.text((20, 42), "LEFT: Magic Lantern Engine  |  CENTER: Reference (Dehancer Desktop)  |  RIGHT: Delta E 2000 Heatmap", fill=(160, 160, 160))
    
    # Footer Stats Summary
    footer_y = banner_h + h + 12
    draw.line([(0, banner_h + h), (total_w, banner_h + h)], fill=(45, 47, 52), width=2)
    
    skin_mid = next((r for r in results if r.zone_name == "skin_midtone"), None)
    skin_hi = next((r for r in results if r.zone_name == "skin_highlight"), None)
    sky = next((r for r in results if "sky" in r.zone_name), None)
    
    col1 = (
        f"SKIN MIDTONES:\n"
        f"  ΔE00: {skin_mid.mean_de00:.2f} (p95: {skin_mid.p95_de00:.2f})\n"
        f"  Hue: Eng={skin_mid.engine_hue_deg:.1f}° vs Ref={skin_mid.ref_hue_deg:.1f}° (Δ={skin_mid.delta_hue_deg:+.1f}°)\n"
        f"  Chroma: {skin_mid.chroma_ratio*100:.1f}% | R-G: Eng={skin_mid.engine_rg_diff:.1f} vs Ref={skin_mid.ref_rg_diff:.1f}"
    ) if skin_mid else "SKIN MIDTONES: No sufficient pixels detected"
    
    col2 = (
        f"SKIN HIGHLIGHTS:\n"
        f"  ΔE00: {skin_hi.mean_de00:.2f} (p95: {skin_hi.p95_de00:.2f})\n"
        f"  Hue: Eng={skin_hi.engine_hue_deg:.1f}° vs Ref={skin_hi.ref_hue_deg:.1f}° (Δ={skin_hi.delta_hue_deg:+.1f}°)\n"
        f"  Diagnostic: {skin_hi.diagnostic}"
    ) if skin_hi else "SKIN HIGHLIGHTS: No sufficient pixels detected"
    
    col3 = (
        f"SKY / ATMOSPHERE:\n"
        f"  ΔE00: {sky.mean_de00:.2f} (p95: {sky.p95_de00:.2f})\n"
        f"  Hue: Eng={sky.engine_hue_deg:.1f}° vs Ref={sky.ref_hue_deg:.1f}° (Δ={sky.delta_hue_deg:+.1f}°)\n"
        f"  Diagnostic: {sky.diagnostic}"
    ) if sky else "SKY: No sufficient pixels detected"
    
    draw.text((25, footer_y), col1, fill=(220, 220, 220))
    draw.text((w * 1 + 25, footer_y), col2, fill=(220, 220, 220))
    if total_w >= w * 3:
        draw.text((w * 2 + 25, footer_y), col3, fill=(220, 220, 220))
        
    return canvas

# ==============================================================================
# PIPELINE EXECUTION (STANDALONE / HOSTTEST WRAPPER)
# ==============================================================================

def render_with_engine(
    grade_bin: Path,
    input_jpg: Path,
    recipe_fs: Path,
    output_jpg: Path
) -> bool:
    """Executes the C engine host test binary to grade a baseline JPEG with .fs recipe."""
    if not grade_bin.exists():
        raise FileNotFoundError(f"Engine binary not found: {grade_bin}")
    if not recipe_fs.exists():
        raise FileNotFoundError(f"Recipe file not found: {recipe_fs}")
        
    output_jpg.parent.mkdir(parents=True, exist_ok=True)
    cmd = [str(grade_bin), str(input_jpg), str(recipe_fs), str(output_jpg)]
    res = subprocess.run(cmd, capture_output=True, text=True)
    return res.returncode == 0 and output_jpg.exists()

# ==============================================================================
# CLI INTERFACE & MAIN SPECIFICATION
# ==============================================================================

def build_cli_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Magic Lantern Film Sim vs Reference Color Comparison Suite",
        formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("--stock", type=str, default="Portra 400", help="Film stock to test (e.g. 'Portra 400', 'Gold 200')")
    p.add_argument("--recipe", type=Path, default=None, help="Path to .fs recipe (defaults to magiclantern/modules/film_sim/films/<stock>.fs)")
    p.add_argument("--ref-dir", type=Path, default=Path("xpan_comparisons/stocks"), help="Directory containing reference renders")
    p.add_argument("--frames", type=str, default="008,001", help="Comma-separated frame IDs to test (e.g. '008,001,006')")
    p.add_argument("--generate-heatmaps", action="store_true", help="Generate CIEDE2000 per-pixel heatmap boards")
    p.add_argument("--out-dir", type=Path, default=Path("film_eval_output"), help="Output directory for reports and comparison images")
    p.add_argument("--format", choices=["markdown", "json", "both"], default="markdown", help="Report output format")
    return p

if __name__ == "__main__":
    # DO NOT EXECUTE YET: Code structure and evaluation criteria presented to user for review.
    print("[INFO] Comparison script defined successfully.")
    print("[INFO] Execution paused pending user review of the evaluation criteria.")

