import sys
import numpy as np

sys.path.insert(0, 'tools')
from compare_film_stocks import srgb_to_lab, ciede2000
from optimize_stock_triad import collect_stock_data, parse_fs, expand_to_48, rgb_to_yuv, yuv_to_rgb, get_sector_indices

def apply_sim(rgb, fs_file):
    fs_path = f"magiclantern/modules/film_sim/films/{fs_file}"
    parsed, _ = parse_fs(fs_path)
    rot = expand_to_48(parsed.get('HUE_ROT', '0'))
    hsat = expand_to_48(parsed.get('HUE_SAT', '100')) / 100.0
    
    y, u, v = rgb_to_yuv(rgb)
    sec = get_sector_indices(rgb)
    
    # Apply rotation and saturation per sector
    rad = np.deg2rad(rot[sec])
    sat = hsat[sec]
    
    u_rot = (u * np.cos(rad) - v * np.sin(rad)) * sat
    v_rot = (u * np.sin(rad) + v * np.cos(rad)) * sat
    
    return yuv_to_rgb(y, u_rot, v_rot)

stocks = [
    ('Portra-160', 'P160.fs'),
    ('Velvia-50', 'velvia.fs'),
    ('Cinestill-50D', '50D.fs'),
    ('Gold-200', 'gold.fs'),
    ('Portra-400', 'portra.fs'),
    ('Ektar-100', 'EK100.fs')
]

print(f"{'Stock':16s} | {'Skin dE (Old->New)':20s} | {'Sea dE (Old->New)':20s} | {'Forest dE (Old->New)':20s}")
print("-" * 84)

results = []

for sname, fs_file in stocks:
    (s_deh, s_ml), (w_deh, w_ml), (f_deh, f_ml) = collect_stock_data(sname)
    
    # Old
    old_s_de = ciede2000(srgb_to_lab(s_deh), srgb_to_lab(s_ml)).mean() if len(s_deh) else 0.0
    old_w_de = ciede2000(srgb_to_lab(w_deh), srgb_to_lab(w_ml)).mean() if len(w_deh) else 0.0
    old_f_de = ciede2000(srgb_to_lab(f_deh), srgb_to_lab(f_ml)).mean() if len(f_deh) else 0.0
    
    # New
    new_s_ml = apply_sim(s_ml, fs_file) if len(s_ml) else s_ml
    new_w_ml = apply_sim(w_ml, fs_file) if len(w_ml) else w_ml
    new_f_ml = apply_sim(f_ml, fs_file) if len(f_ml) else f_ml
    
    new_s_de = ciede2000(srgb_to_lab(s_deh), srgb_to_lab(new_s_ml)).mean() if len(s_deh) else 0.0
    new_w_de = ciede2000(srgb_to_lab(w_deh), srgb_to_lab(new_w_ml)).mean() if len(w_deh) else 0.0
    new_f_de = ciede2000(srgb_to_lab(f_deh), srgb_to_lab(new_f_ml)).mean() if len(f_deh) else 0.0
    
    # Vectors
    d_s_rb = (s_deh[:,0] - s_deh[:,2]).mean() if len(s_deh) else 0
    new_s_rb = (new_s_ml[:,0] - new_s_ml[:,2]).mean() if len(s_ml) else 0
    d_w_br = (w_deh[:,2] - w_deh[:,0]).mean() if len(w_deh) else 0
    new_w_br = (new_w_ml[:,2] - new_w_ml[:,0]).mean() if len(w_ml) else 0
    
    print(f"{sname:16s} | {old_s_de:5.2f} -> {new_s_de:5.2f} (R-B:+{new_s_rb:.0f}) | {old_w_de:5.2f} -> {new_w_de:5.2f} (B-R:+{new_w_br:.0f}) | {old_f_de:5.2f} -> {new_f_de:5.2f}")
    results.append((sname, old_s_de, new_s_de, old_w_de, new_w_de, old_f_de, new_f_de, new_s_rb, d_s_rb, new_w_br, d_w_br))
