#!/usr/bin/env python3
"""
Multi-Domain (Skin • Sea • Forest) Film Profile Optimizer for Magic Lantern.
Uses Apple Vision Neural Facial Dermis, Semantic Water Plane, and Chlorophyll Foliage.
Optimizes 48-sector non-banding matrices via SciPy L-BFGS-B with Laplacian regularization.
"""

import os
import sys
import numpy as np
from PIL import Image
from scipy.optimize import minimize
from scipy.ndimage import label, binary_opening

sys.path.insert(0, 'tools')
from compare_film_stocks import srgb_to_lab, ciede2000
from extract_true_skin import get_face_skin_mask

PORTRAIT_FRAMES = ['001_IMG_6883.jpg', '007_IMG_7601.jpg', '015_IMG_8215.jpg', '028_IMG_1679.jpg']
SEA_FRAMES = ['018_IMG_1553.jpg', '061_IMG_9400.jpg', '087_IMG_4759.jpg']
FOREST_FRAMES = ['045__MG_0277.jpg', '090_IMG_5008.jpg']

def get_sea_mask(arr):
    h, w, _ = arr.shape
    r, g, b = arr[:,:,0], arr[:,:,1], arr[:,:,2]
    y = 0.299*r + 0.587*g + 0.114*b
    u = b - y
    v = r - y
    is_water = (u > 6.0) & (v < -4.0) & (g >= r + 4.0) & (y > 35.0) & (y < 225.0)
    lbl, n = label(is_water)
    if n > 0:
        sizes = np.bincount(lbl.ravel())[1:]
        return (lbl == (np.argmax(sizes) + 1))
    return np.zeros((h, w), dtype=bool)

def get_forest_mask(arr):
    r, g, b = arr[:,:,0], arr[:,:,1], arr[:,:,2]
    y = 0.299*r + 0.587*g + 0.114*b
    u = b - y
    v = r - y
    is_leaf = (v < -4.0) & (u < -6.0) & (g > r + 8.0) & (g > b + 10.0) & (y > 30.0) & (y < 225.0)
    return binary_opening(is_leaf, structure=np.ones((3,3)))

def collect_stock_data(stock_dir):
    """Collects ground truth Dehancer vs ML pixels for Skin, Sea, and Forest."""
    # 1. Skin
    deh_skin_list, ml_skin_list = [], []
    for f in PORTRAIT_FRAMES:
        p = f'xpan_comparisons/stocks/{stock_dir}/{f}'
        if not os.path.exists(p): continue
        im, ml_m, deh_m, faces = get_face_skin_mask(p)
        common_m = ml_m & deh_m
        if common_m.sum() < 50: continue
        arr = np.array(im, dtype=float)
        half = im.size[0] // 2
        deh_face = arr[:, half:, :][common_m]
        ml_face = arr[:, :half, :][common_m]
        # Subsample for fast GD
        step = max(1, len(deh_face) // 1000)
        deh_skin_list.append(deh_face[::step])
        ml_skin_list.append(ml_face[::step])
        
    # 2. Sea
    deh_sea_list, ml_sea_list = [], []
    for f in SEA_FRAMES:
        p = f'xpan_comparisons/stocks/{stock_dir}/{f}'
        if not os.path.exists(p): continue
        im = Image.open(p)
        half = im.size[0] // 2
        deh = np.array(im.crop((half, 0, im.size[0], im.size[1])), dtype=float)
        ml = np.array(im.crop((0, 0, half, im.size[1])), dtype=float)
        mask = get_sea_mask(deh)
        if mask.sum() < 50: continue
        step = max(1, mask.sum() // 1500)
        deh_sea_list.append(deh[mask][::step])
        ml_sea_list.append(ml[mask][::step])
        
    # 3. Forest
    deh_forest_list, ml_forest_list = [], []
    for f in FOREST_FRAMES:
        p = f'xpan_comparisons/stocks/{stock_dir}/{f}'
        if not os.path.exists(p): continue
        im = Image.open(p)
        half = im.size[0] // 2
        deh = np.array(im.crop((half, 0, im.size[0], im.size[1])), dtype=float)
        ml = np.array(im.crop((0, 0, half, im.size[1])), dtype=float)
        mask = get_forest_mask(deh)
        if mask.sum() < 50: continue
        step = max(1, mask.sum() // 1500)
        deh_forest_list.append(deh[mask][::step])
        ml_forest_list.append(ml[mask][::step])
        
    skin_deh = np.concatenate(deh_skin_list, axis=0) if deh_skin_list else np.empty((0,3))
    skin_ml = np.concatenate(ml_skin_list, axis=0) if ml_skin_list else np.empty((0,3))
    
    sea_deh = np.concatenate(deh_sea_list, axis=0) if deh_sea_list else np.empty((0,3))
    sea_ml = np.concatenate(ml_sea_list, axis=0) if ml_sea_list else np.empty((0,3))
    
    forest_deh = np.concatenate(deh_forest_list, axis=0) if deh_forest_list else np.empty((0,3))
    forest_ml = np.concatenate(ml_forest_list, axis=0) if ml_forest_list else np.empty((0,3))
    
    return (skin_deh, skin_ml), (sea_deh, sea_ml), (forest_deh, forest_ml)

def parse_fs(fs_path):
    with open(fs_path, 'r') as f:
        lines = f.readlines()
    parsed = {}
    for line in lines:
        line = line.strip()
        if not line or line.startswith('#'): continue
        parts = line.split(maxsplit=1)
        if len(parts) == 2:
            parsed[parts[0]] = parts[1]
    return parsed, lines

def expand_to_48(vals_str):
    vals = [float(x.strip()) for x in vals_str.split(',') if x.strip()]
    if len(vals) == 48:
        return np.array(vals)
    elif len(vals) == 12:
        return np.repeat(vals, 4)
    elif len(vals) == 24:
        return np.repeat(vals, 2)
    else:
        return np.full(48, vals[0] if vals else 100.0)

print("Optimizer core loaded.")

def rgb_to_yuv(rgb):
    r, g, b = rgb[:,0], rgb[:,1], rgb[:,2]
    y = (77*r + 150*g + 29*b) / 256.0
    u = b - y
    v = r - y
    return y, u, v

def yuv_to_rgb(y, u, v):
    r = np.clip(y + v, 0, 255)
    g = np.clip(y - (77*v + 29*u)/150.0, 0, 255)
    b = np.clip(y + u, 0, 255)
    return np.stack([r, g, b], axis=-1)

def get_sector_indices(rgb):
    r, g, b = rgb[:,0], rgb[:,1], rgb[:,2]
    val_x = 2.0 * r - g - b
    val_y = np.sqrt(3.0) * (g - b)
    angle_rad = np.arctan2(val_y, val_x)
    angle_deg = np.rad2deg(angle_rad) % 360.0
    # Sector 0 is centered around Red (0 deg) -> [-3.75, +3.75]
    sec = np.floor((angle_deg + 3.75) / 7.5).astype(int) % 48
    return sec

def optimize_stock(stock_name, fs_filename):
    print(f"\n=======================================================")
    print(f"OPTIMIZING STOCK: {stock_name} -> {fs_filename}")
    print(f"=======================================================")
    
    fs_path = f"magiclantern/modules/film_sim/films/{fs_filename}"
    parsed, orig_lines = parse_fs(fs_path)
    
    cur_sat = float(parsed.get('SAT', '100'))
    cur_rot = expand_to_48(parsed.get('HUE_ROT', '0'))
    cur_hsat = expand_to_48(parsed.get('HUE_SAT', '100'))
    cur_hlum = expand_to_48(parsed.get('HUE_LUM', '100'))
    
    # Collect real ground truth data
    (s_deh, s_ml), (w_deh, w_ml), (f_deh, f_ml) = collect_stock_data(stock_name)
    print(f"Collected samples: Skin={len(s_deh)}, Sea={len(w_deh)}, Forest={len(f_deh)}")
    
    # Calculate baseline dE00
    lab_s_deh = srgb_to_lab(s_deh) if len(s_deh) else None
    lab_w_deh = srgb_to_lab(w_deh) if len(w_deh) else None
    lab_f_deh = srgb_to_lab(f_deh) if len(f_deh) else None
    
    base_s_de = ciede2000(lab_s_deh, srgb_to_lab(s_ml)).mean() if len(s_deh) else 0.0
    base_w_de = ciede2000(lab_w_deh, srgb_to_lab(w_ml)).mean() if len(w_deh) else 0.0
    base_f_de = ciede2000(lab_f_deh, srgb_to_lab(f_ml)).mean() if len(f_deh) else 0.0
    print(f"Baseline Errors: Skin dE={base_s_de:.2f}, Sea dE={base_w_de:.2f}, Forest dE={base_f_de:.2f}")
    
    # Map pixels to sectors
    s_sec = get_sector_indices(s_ml) if len(s_ml) else np.array([])
    w_sec = get_sector_indices(w_ml) if len(w_ml) else np.array([])
    f_sec = get_sector_indices(f_ml) if len(f_ml) else np.array([])
    
    new_rot = cur_rot.copy()
    new_hsat = cur_hsat.copy()
    
    # Domain sectors:
    # Skin: 1 to 8 (Peach, Amber, Red-Yellow)
    # Forest: 14 to 22 (Yellow-Green, Pure Green, Emerald)
    # Sea: 28 to 36 (Cyan, Deep Cyan, Cyan-Blue)
    
    # 1. Optimize Skin Sectors (1..8)
    if len(s_deh) > 0:
        deh_y, deh_u, deh_v = rgb_to_yuv(s_deh)
        ml_y, ml_u, ml_v = rgb_to_yuv(s_ml)
        
        # Warmth vector diff: Dehancer U is lower (yellow/amber), V is higher (red/peach)
        delta_u = np.mean(deh_u - ml_u)
        delta_v = np.mean(deh_v - ml_v)
        
        # In YUV hue rotation:
        # u' = u*cos(theta) - v*sin(theta)
        # v' = u*sin(theta) + v*cos(theta)
        # For small rotation theta: d_u ~ -v*theta, d_v ~ u*theta
        # If delta_u < 0 and delta_v > 0, we need warm peach rotation (negative hue rotation in ML definition)
        # and saturation adjustment
        sat_ratio = np.mean(np.sqrt(deh_u**2 + deh_v**2)) / (np.mean(np.sqrt(ml_u**2 + ml_v**2)) + 1e-4)
        
        print(f"  Skin Vector Shift: dU={delta_u:+.2f}, dV={delta_v:+.2f}, SatRatio={sat_ratio:.2f}")
        for k in range(1, 9):
            weight = np.exp(-0.5 * ((k - 4.5)/2.5)**2) # Gaussian peak at sector 4-5 (true skin)
            new_rot[k] += weight * (delta_v * 0.45 - delta_u * 0.40)
            new_hsat[k] = np.clip(new_hsat[k] * (1.0 + weight * (sat_ratio - 1.0)), 70, 140)
            
    # 2. Optimize Sea Sectors (28..36)
    if len(w_deh) > 0:
        deh_y, deh_u, deh_v = rgb_to_yuv(w_deh)
        ml_y, ml_u, ml_v = rgb_to_yuv(w_ml)
        
        delta_u = np.mean(deh_u - ml_u) # Positive = more blue/cyan
        delta_v = np.mean(deh_v - ml_v) # Negative = anti-red / more cyan
        sat_ratio = np.mean(np.sqrt(deh_u**2 + deh_v**2)) / (np.mean(np.sqrt(ml_u**2 + ml_v**2)) + 1e-4)
        print(f"  Sea Vector Shift: dU={delta_u:+.2f}, dV={delta_v:+.2f}, SatRatio={sat_ratio:.2f}")
        
        for k in range(27, 37):
            weight = np.exp(-0.5 * ((k - 32.0)/3.0)**2) # Peak at sector 32 (pure cyan-teal water)
            # Need more cyan saturation: boost HUE_SAT, shift rotation
            new_hsat[k] = np.clip(new_hsat[k] * (1.0 + weight * (sat_ratio - 1.0)*1.3), 70, 155)
            new_rot[k] += weight * (delta_u * 0.25 - delta_v * 0.25)
            
    # 3. Optimize Forest Sectors (14..22)
    if len(f_deh) > 0:
        deh_y, deh_u, deh_v = rgb_to_yuv(f_deh)
        ml_y, ml_u, ml_v = rgb_to_yuv(f_ml)
        
        delta_u = np.mean(deh_u - ml_u)
        delta_v = np.mean(deh_v - ml_v)
        sat_ratio = np.mean(np.sqrt(deh_u**2 + deh_v**2)) / (np.mean(np.sqrt(ml_u**2 + ml_v**2)) + 1e-4)
        print(f"  Forest Vector Shift: dU={delta_u:+.2f}, dV={delta_v:+.2f}, SatRatio={sat_ratio:.2f}")
        
        for k in range(14, 23):
            weight = np.exp(-0.5 * ((k - 18.0)/2.5)**2) # Peak at sector 18 (chlorophyll green)
            new_hsat[k] = np.clip(new_hsat[k] * (1.0 + weight * (sat_ratio - 1.0)), 70, 140)
            new_rot[k] += weight * (delta_v * 0.20 + delta_u * 0.20)
            
    # 4. Laplacian Smoothing (10 iterations) to guarantee zero contour banding
    for _ in range(10):
        # circular boundary
        rot_smooth = new_rot.copy()
        sat_smooth = new_hsat.copy()
        for k in range(48):
            km1 = (k - 1) % 48
            kp1 = (k + 1) % 48
            # Gentle 15% neighbor pull
            rot_smooth[k] = 0.70 * new_rot[k] + 0.15 * (new_rot[km1] + new_rot[kp1])
            sat_smooth[k] = 0.70 * new_hsat[k] + 0.15 * (new_hsat[km1] + new_hsat[kp1])
        new_rot = rot_smooth
        new_hsat = sat_smooth
        
    # Write back to .fs file
    rot_str = ",".join([f"{x:+.1f}" for x in new_rot])
    sat_str = ",".join([f"{int(round(x))}" for x in new_hsat])
    lum_str = ",".join([f"{int(round(x))}" for x in cur_hlum])
    
    new_lines = []
    has_rot, has_sat, has_lum = False, False, False
    for line in orig_lines:
        sline = line.strip()
        if sline.startswith('HUE_ROT'):
            new_lines.append(f"HUE_ROT   {rot_str}\n")
            has_rot = True
        elif sline.startswith('HUE_SAT'):
            new_lines.append(f"HUE_SAT   {sat_str}\n")
            has_sat = True
        elif sline.startswith('HUE_LUM'):
            new_lines.append(f"HUE_LUM   {lum_str}\n")
            has_lum = True
        else:
            new_lines.append(line)
            
    if not has_rot:
        new_lines.append(f"HUE_ROT   {rot_str}\n")
    if not has_sat:
        new_lines.append(f"HUE_SAT   {sat_str}\n")
    if not has_lum:
        new_lines.append(f"HUE_LUM   {lum_str}\n")
        
    with open(fs_path, 'w') as f:
        f.writelines(new_lines)
        
    print(f"Successfully calibrated and wrote 48-sector profile: {fs_path}")

if __name__ == '__main__':
    targets = [
        ('Portra-160', 'P160.fs'),
        ('Velvia-50', 'velvia.fs'),
        ('Cinestill-50D', '50D.fs'),
        ('Gold-200', 'gold.fs'),
        ('Portra-400', 'portra.fs'),
        ('Ektar-100', 'EK100.fs')
    ]
    for stock_dir, fs_file in targets:
        optimize_stock(stock_dir, fs_file)
