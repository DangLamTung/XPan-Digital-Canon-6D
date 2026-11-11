#!/usr/bin/env python3
"""Build side-by-side Dehancer vs Magic Lantern benchmark boards for real photographic frames."""

import os
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, 'tools')
from compare_film_stocks import srgb_to_lab, ciede2000

ARTIFACT_DIR = '/Users/tungdl/.gemini/antigravity/brain/b15af59f-a878-4ed2-99f5-5e2a2fe2371a'

def make_board(stock_dir, frame_file, title, skin_box=None):
    img_path = f'xpan_comparisons/stocks/{stock_dir}/{frame_file}'
    if not os.path.exists(img_path):
        print(f'File not found: {img_path}')
        return
    
    im = Image.open(img_path).convert('RGB')
    w, h = im.size
    half = w // 2
    
    ml_img = im.crop((0, 0, half, h))
    deh_img = im.crop((half, 0, w, h))
    
    arr_ml = np.array(ml_img, dtype=float)
    arr_deh = np.array(deh_img, dtype=float)
    
    lab_ml = srgb_to_lab(arr_ml)
    lab_deh = srgb_to_lab(arr_deh)
    de_map = ciede2000(lab_ml, lab_deh)
    
    mean_de = float(np.mean(de_map))
    median_de = float(np.median(de_map))
    p90_de = float(np.percentile(de_map, 90))
    
    # False color heatmap: 0=Navy, 2=Cyan, 4=Green, 7=Yellow, 10+=Red/White
    norm_de = np.clip(de_map / 10.0, 0.0, 1.0)
    r = np.clip(2.0 * norm_de - 0.5, 0.0, 1.0)
    g = np.clip(1.0 - 2.0 * np.abs(norm_de - 0.5), 0.0, 1.0)
    b = np.clip(1.0 - 2.0 * norm_de, 0.0, 1.0)
    heatmap_arr = (np.stack([r, g, b], axis=-1) * 255.0).astype(np.uint8)
    heatmap_img = Image.fromarray(heatmap_arr)
    
    # Board layout:
    # Header: 80px
    # Row 1: ML (700x484) | Dehancer (700x484)
    # Row 2: Heatmap (700x484) | Crops / Split Zoom (700x484)
    # Footer: 90px
    board_w = half * 2
    board_h = h * 2 + 170
    
    board = Image.new('RGB', (board_w, board_h), (20, 21, 24))
    draw = ImageDraw.Draw(board)
    
    # Header
    draw.rectangle([0, 0, board_w, 75], fill=(15, 16, 18))
    draw.text((25, 18), f'REAL IMAGE BENCHMARK: CineStill 50D — {title} ({frame_file})', fill=(255, 220, 130))
    draw.text((25, 45), f'Global Frame Metrics: Mean dE00 = {mean_de:.2f} | Median dE00 = {median_de:.2f} | 90th percentile = {p90_de:.2f}', fill=(180, 180, 185))
    
    # Row 1: Images
    board.paste(ml_img, (0, 75))
    board.paste(deh_img, (half, 75))
    draw.line([(half, 75), (half, 75 + h)], fill=(60, 60, 65), width=2)
    
    # Labeled overlay tags
    draw.rectangle([10, 85, 270, 115], fill=(0, 0, 0, 180))
    draw.text((20, 92), 'LEFT: Magic Lantern Engine', fill=(240, 240, 240))
    
    draw.rectangle([half + 10, 85, half + 300, 115], fill=(0, 0, 0, 180))
    draw.text((half + 20, 92), 'RIGHT: Dehancer Desktop (CineStill 50D)', fill=(100, 220, 255))
    
    # Row 2 Left: Heatmap
    board.paste(heatmap_img, (0, 75 + h + 10))
    draw.rectangle([10, 75 + h + 20, 280, 75 + h + 50], fill=(0, 0, 0, 180))
    draw.text((20, 75 + h + 27), 'Delta E 2000 Heatmap (Dark=0, Green=4, Red=10+)', fill=(240, 240, 240))
    
    # Row 2 Right: Split 100% Zoom / Crop
    crop_panel = Image.new('RGB', (half, h), (18, 18, 20))
    draw_crop = ImageDraw.Draw(crop_panel)
    
    if skin_box is not None:
        # Crop to box: skin_box is (x1, y1, x2, y2) in 0..1 relative coords
        bx1, by1, bx2, by2 = int(skin_box[0]*half), int(skin_box[1]*h), int(skin_box[2]*half), int(skin_box[3]*h)
        crop_ml = ml_img.crop((bx1, by1, bx2, by2)).resize((half, h // 2), Image.LANCZOS)
        crop_deh = deh_img.crop((bx1, by1, bx2, by2)).resize((half, h // 2), Image.LANCZOS)
        
        # Upper crop: Dehancer
        crop_panel.paste(crop_deh, (0, 0))
        # Lower crop: ML
        crop_panel.paste(crop_ml, (0, h // 2))
        
        draw_crop.line([(0, h // 2), (half, h // 2)], fill=(255, 255, 255), width=2)
        draw_crop.text((15, 15), 'ZOOM DETAIL: TOP = Dehancer Desktop', fill=(255, 255, 255))
        draw_crop.text((15, h // 2 + 15), 'ZOOM DETAIL: BOTTOM = Magic Lantern Engine', fill=(255, 255, 255))
    else:
        # Default split crop around center
        cx1, cy1, cx2, cy2 = int(half*0.25), int(h*0.25), int(half*0.75), int(h*0.75)
        crop_ml = ml_img.crop((cx1, cy1, cx2, cy2)).resize((half, h // 2), Image.LANCZOS)
        crop_deh = deh_img.crop((cx1, cy1, cx2, cy2)).resize((half, h // 2), Image.LANCZOS)
        crop_panel.paste(crop_deh, (0, 0))
        crop_panel.paste(crop_ml, (0, h // 2))
        draw_crop.line([(0, h // 2), (half, h // 2)], fill=(255, 255, 255), width=2)
        draw_crop.text((15, 15), 'ZOOM DETAIL: TOP = Dehancer Desktop', fill=(255, 255, 255))
        draw_crop.text((15, h // 2 + 15), 'ZOOM DETAIL: BOTTOM = Magic Lantern Engine', fill=(255, 255, 255))
        
    board.paste(crop_panel, (half, 75 + h + 10))
    
    # Footer
    footer_y = 75 + h * 2 + 25
    draw.line([(0, 75 + h * 2 + 15), (board_w, 75 + h * 2 + 15)], fill=(50, 50, 55), width=2)
    draw.text((25, footer_y), 'Evaluation criteria: Skin tones match within ±2° hue. Sky and shadow tones hold characteristic CineStill 50D cyan-teal tint.', fill=(160, 160, 165))
    draw.text((25, footer_y + 25), 'Reference export generated by Dehancer Desktop 1.1.0 using Kodak Endura Glossy Paper profile.', fill=(130, 130, 135))
    
    out_file = f'{ARTIFACT_DIR}/dehancer_real_test_{title.lower().replace(" ", "_")}.jpg'
    board.save(out_file, quality=95)
    print(f'Saved benchmark board: {out_file}')

if __name__ == '__main__':
    # 1. Girl Portrait (Skin tones, hair, highlight rolloff)
    make_board('Cinestill-50D', '001_IMG_6883.jpg', 'Portrait_Girl', skin_box=(0.28, 0.05, 0.62, 0.55))
    # 2. Street Scene (Mixed lighting, people, pavement, foliage)
    make_board('Cinestill-50D', '008_IMG_7680.jpg', 'Street_Atmosphere', skin_box=(0.35, 0.20, 0.65, 0.70))
    # 3. Outdoor Portrait / Scenery
    make_board('Cinestill-50D', '015_IMG_8215.jpg', 'Outdoor_Sunlight', skin_box=(0.30, 0.15, 0.70, 0.65))
    # 4. Landscape / Nature
    make_board('Cinestill-50D', '006_IMG_7517-2.jpg', 'Landscape_Nature')

