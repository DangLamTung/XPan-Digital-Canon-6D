#!/usr/bin/env python3
import json
import subprocess
import os
import sys
import numpy as np
from PIL import Image, ImageDraw

def get_face_skin_mask(img_path):
    """
    Uses Apple Vision framework via tools/detect_faces to extract
    true anatomical facial dermis masks for both ML (left) and Dehancer (right).
    Returns:
        im: PIL Image (full 1400x484)
        ml_mask: bool array (484, 700)
        deh_mask: bool array (484, 700)
        face_list: list of dicts with bounding boxes and landmarks
    """
    res = subprocess.run(['tools/detect_faces', img_path], capture_output=True, text=True)
    if res.returncode != 0 or not res.stdout.strip():
        # Fallback empty
        im = Image.open(img_path)
        h, w = im.size[1], im.size[0] // 2
        return im, np.zeros((h, w), dtype=bool), np.zeros((h, w), dtype=bool), []
    
    # Parse json output (ignore any debug lines before '[')
    stdout = res.stdout
    idx = stdout.find('[')
    if idx == -1:
        im = Image.open(img_path)
        h, w = im.size[1], im.size[0] // 2
        return im, np.zeros((h, w), dtype=bool), np.zeros((h, w), dtype=bool), []
        
    faces = json.loads(stdout[idx:])
    
    im = Image.open(img_path)
    total_w, total_h = im.size
    half_w = total_w // 2
    
    ml_mask_img = Image.new('1', (half_w, total_h), 0)
    deh_mask_img = Image.new('1', (half_w, total_h), 0)
    
    draw_ml = ImageDraw.Draw(ml_mask_img)
    draw_deh = ImageDraw.Draw(deh_mask_img)
    
    for face in faces:
        bbox = face['bbox']
        is_deh = face['is_dehancer_half']
        draw = draw_deh if is_deh else draw_ml
        x_offset = half_w if is_deh else 0
        
        lms = face.get('landmarks', {})
        if 'faceContour' in lms and len(lms['faceContour']) >= 3:
            # Draw face polygon
            pts = [(pt['x'] - x_offset, pt['y']) for pt in lms['faceContour']]
            draw.polygon(pts, fill=1)
            
            # Subtract eyes
            if 'leftEye' in lms and len(lms['leftEye']) >= 3:
                e_pts = [(pt['x'] - x_offset, pt['y']) for pt in lms['leftEye']]
                draw.polygon(e_pts, fill=0)
            if 'rightEye' in lms and len(lms['rightEye']) >= 3:
                e_pts = [(pt['x'] - x_offset, pt['y']) for pt in lms['rightEye']]
                draw.polygon(e_pts, fill=0)
                
            # Subtract lips
            if 'outerLips' in lms and len(lms['outerLips']) >= 3:
                l_pts = [(pt['x'] - x_offset, pt['y']) for pt in lms['outerLips']]
                draw.polygon(l_pts, fill=0)
        else:
            # Fallback: inner 70% of face bounding box
            bx, by, bw, bh = bbox
            rx1 = bx - x_offset + bw * 0.15
            ry1 = by + bh * 0.20
            rx2 = bx - x_offset + bw * 0.85
            ry2 = by + bh * 0.85
            draw.rectangle([rx1, ry1, rx2, ry2], fill=1)
            
    ml_mask = np.array(ml_mask_img, dtype=bool)
    deh_mask = np.array(deh_mask_img, dtype=bool)
    
    return im, ml_mask, deh_mask, faces

if __name__ == '__main__':
    for test_file in ['001_IMG_6883.jpg', '002_IMG_6888.jpg', '008_IMG_7680.jpg', '015_IMG_8215.jpg', '018_IMG_1553.jpg']:
        p = f'xpan_comparisons/stocks/Portra-160/{test_file}'
        im, ml_m, deh_m, faces = get_face_skin_mask(p)
        print(f'{test_file}: {len(faces)} faces detected. ML mask: {ml_m.sum()} px, Deh mask: {deh_m.sum()} px')
