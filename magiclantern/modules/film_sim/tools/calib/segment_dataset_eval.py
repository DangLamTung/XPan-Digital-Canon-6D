#!/usr/bin/env python3
"""
Segment-Aware Diagnostic Evaluation & Analysis on 10-Image Dataset.
Segments Skin, Hair, Lips, Eyes, Sea, Foliage, Highlights, Midtones, and Shadows
to analyze exact CIEDE2000 color errors and guide optimal Style Engine calibration.
"""

import os
import sys
import subprocess
import numpy as np
import cv2
from PIL import Image, ImageDraw

import mediapipe as mp
from mediapipe.tasks import python
from mediapipe.tasks.python import vision

BASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../../../.."))
GRADE_BIN = os.path.join(BASE_DIR, "magiclantern/modules/film_sim/se/hosttest/grade_jpeg")
TARGET_FS = os.path.join(BASE_DIR, "magiclantern/modules/film_sim/films/50D.fs")
ARTIFACT_DIR = "/Users/tungdl/.gemini/antigravity/brain/b15af59f-a878-4ed2-99f5-5e2a2fe2371a"

sys.path.append(os.path.dirname(__file__))
from tune_ciede2000 import srgb_to_lab, ciede2000

# 10 Diverse Dataset Images
DATASET = [
    ("001_IMG_6883.jpg", "Street Portrait", "skin_portrait"),
    ("015_IMG_8215.jpg", "Close-up Portrait", "skin_portrait"),
    ("029_IMG_1693.jpg", "Warm Low-Key Portrait", "skin_portrait"),
    ("018_IMG_1553.jpg", "Coastline Marine Sea", "sea_water"),
    ("026_IMG_1354.jpg", "Open Ocean & Blue Sky", "sea_water"),
    ("008_IMG_7680.jpg", "Landscape Foliage & Sky Highlights", "foliage_highlights"),
    ("046__MG_0301.jpg", "Dense Forest Chlorophyll Green", "foliage"),
    ("016_IMG_8601.jpg", "High Dynamic Range Highlights", "highlights"),
    ("054_IMG_6176.jpg", "Urban Midtones & Architecture", "midtones"),
    ("060_IMG_7345.jpg", "Deep Shadow & Dark Grays", "shadows")
]

# MediaPipe Landmark Indices
LIPS_INDEX = [61, 146, 91, 181, 84, 17, 314, 405, 321, 375, 291, 308, 324, 318, 402, 317, 14, 87, 178, 88, 95, 185, 40, 39, 37, 0, 267, 269, 270, 409, 415, 310, 311, 312, 13, 82, 81, 42, 183, 78]
LEFT_EYE_INDEX = [263, 249, 390, 373, 374, 380, 381, 382, 362, 263, 466, 388, 387, 386, 385, 384, 398]
RIGHT_EYE_INDEX = [33, 7, 163, 144, 145, 153, 154, 155, 133, 33, 246, 161, 160, 159, 158, 157, 173]
FACE_OVAL_INDEX = [10, 338, 297, 332, 284, 251, 389, 356, 454, 323, 361, 288, 397, 365, 379, 378, 400, 377, 152, 148, 176, 149, 150, 136, 172, 58, 132, 93, 234, 127, 162, 21, 54, 103, 67, 109]

def init_landmarker():
    model_path = os.path.join(os.path.dirname(__file__), "models/face_landmarker.task")
    base_options = python.BaseOptions(model_asset_path=model_path)
    options = vision.FaceLandmarkerOptions(base_options=base_options, output_face_blendshapes=False)
    return vision.FaceLandmarker.create_from_options(options)

def extract_face_segments(landmarker, img_rgb):
    h, w, _ = img_rgb.shape
    mp_img = mp.Image(image_format=mp.ImageFormat.SRGB, data=img_rgb)
    res = landmarker.detect(mp_img)

    mask_skin = np.zeros((h, w), dtype=np.uint8)
    mask_lips = np.zeros((h, w), dtype=np.uint8)
    mask_eyes = np.zeros((h, w), dtype=np.uint8)
    mask_hair = np.zeros((h, w), dtype=np.uint8)

    if not res.face_landmarks:
        return mask_skin, mask_lips, mask_eyes, mask_hair

    lm = res.face_landmarks[0]
    def get_poly(indices):
        return np.array([[int(lm[i].x * w), int(lm[i].y * h)] for i in indices], dtype=np.int32)

    poly_face = get_poly(FACE_OVAL_INDEX)
    poly_lips = get_poly(LIPS_INDEX)
    poly_leye = get_poly(LEFT_EYE_INDEX)
    poly_reye = get_poly(RIGHT_EYE_INDEX)

    cv2.fillPoly(mask_lips, [poly_lips], 255)
    cv2.fillPoly(mask_eyes, [poly_leye, poly_reye], 255)

    face_full = np.zeros((h, w), dtype=np.uint8)
    cv2.fillPoly(face_full, [poly_face], 255)

    # Skin = Face oval minus eyes minus lips
    mask_skin = cv2.bitwise_and(face_full, cv2.bitwise_not(mask_lips))
    mask_skin = cv2.bitwise_and(mask_skin, cv2.bitwise_not(mask_eyes))

    # Hair estimation: Dilated face bounding box above eyebrows
    min_x, max_x = np.min(poly_face[:, 0]), np.max(poly_face[:, 0])
    min_y, max_y = np.min(poly_face[:, 1]), np.max(poly_face[:, 1])
    hair_top = max(0, min_y - int((max_y - min_y) * 0.45))
    hair_mask_candidate = np.zeros((h, w), dtype=np.uint8)
    hair_mask_candidate[hair_top:min_y + 20, max(0, min_x - 30):min(w, max_x + 30)] = 255
    # Hair pixels are typically darker (Luma < 120)
    luma = 0.299 * img_rgb[..., 0] + 0.587 * img_rgb[..., 1] + 0.114 * img_rgb[..., 2]
    mask_hair = cv2.bitwise_and(hair_mask_candidate, (luma < 120).astype(np.uint8) * 255)
    mask_hair = cv2.bitwise_and(mask_hair, cv2.bitwise_not(face_full))

    return mask_skin, mask_lips, mask_eyes, mask_hair

def extract_general_segments(img_rgb):
    """Segments Sea, Foliage, Highlights, Midtones, and Shadows based on color physics."""
    h, w, _ = img_rgb.shape
    arr = img_rgb.astype(float)
    r, g, b = arr[..., 0], arr[..., 1], arr[..., 2]
    luma = 0.299 * r + 0.587 * g + 0.114 * b

    # Sea / Oceanic Surf: Blue > Red + 15, Green > Red + 5, Luma between 30 and 220
    mask_sea = ((b > r + 15) & (g > r + 5) & (luma > 30) & (luma < 220)).astype(np.uint8) * 255

    # Foliage Green: Green > Red + 10, Green > Blue + 10, Luma between 25 and 180
    mask_foliage = ((g > r + 10) & (g > b + 10) & (luma > 25) & (luma < 180)).astype(np.uint8) * 255

    # Highlights: Luma > 200 or any channel > 225
    mask_hi = ((luma > 195) | (np.maximum(np.maximum(r, g), b) > 225)).astype(np.uint8) * 255

    # Shadows / Dark: Luma < 50
    mask_shadow = (luma < 50).astype(np.uint8) * 255

    # Midtones: 70 <= Luma <= 170 and not foliage and not sea
    mask_mid = ((luma >= 70) & (luma <= 170) & (mask_sea == 0) & (mask_foliage == 0)).astype(np.uint8) * 255

    return mask_sea, mask_foliage, mask_hi, mask_shadow, mask_mid

def main():
    print("=" * 80)
    print("   10-IMAGE DATASET SEGMENTATION & DIAGNOSTIC EVALUATION")
    print("=" * 80)

    landmarker = init_landmarker()

    segmented_metrics = {
        "Skin": [], "Lips": [], "Eyes": [], "Hair": [],
        "Sea": [], "Foliage": [], "Highlights": [], "Midtones": [], "Shadows": []
    }

    for fname, title, category in DATASET:
        src_p = os.path.join(BASE_DIR, f"batch110b/{fname}")
        ref_p = os.path.join(BASE_DIR, f"xpan_comparisons/stocks/Cinestill-50D/{fname}")

        ref_im = Image.open(ref_p)
        w, _ = ref_im.size
        half = w // 2
        h = 466
        deh_ref = ref_im.crop((half, 0, w, h))

        src_im = Image.open(src_p)
        orient = src_im.getexif().get(274, 1)
        if orient in {6: Image.Transpose.ROTATE_270, 8: Image.Transpose.ROTATE_90, 3: Image.Transpose.ROTATE_180}:
            src_im = src_im.transpose({6: Image.Transpose.ROTATE_270, 8: Image.Transpose.ROTATE_90, 3: Image.Transpose.ROTATE_180}[orient])
        src_resized = src_im.resize((half, h), Image.Resampling.LANCZOS)

        tmp_in = f"/tmp/ds_src_{fname}"
        tmp_out = f"/tmp/ds_graded_{fname}"
        src_resized.save(tmp_in, quality=95)

        subprocess.run([GRADE_BIN, tmp_in, TARGET_FS, tmp_out], capture_output=True, check=True)
        ml_graded = Image.open(tmp_out).convert("RGB")

        arr_ml = np.array(ml_graded)
        arr_deh = np.array(deh_ref)

        lab_ml = srgb_to_lab(arr_ml.astype(float))
        lab_deh = srgb_to_lab(arr_deh.astype(float))
        de_map = ciede2000(lab_deh, lab_ml)

        print(f"\n[{fname}] — {title} (Global Mean dE00: {np.mean(de_map):.2f})")

        # 1. Face Segmentation (Skin, Hair, Lips, Eyes)
        mask_skin, mask_lips, mask_eyes, mask_hair = extract_face_segments(landmarker, np.array(src_resized))
        # 2. General Segmentation (Sea, Foliage, Highlights, Shadows, Midtones)
        mask_sea, mask_foliage, mask_hi, mask_shadow, mask_mid = extract_general_segments(arr_deh)

        segments = [
            ("Skin", mask_skin), ("Lips", mask_lips), ("Eyes", mask_eyes), ("Hair", mask_hair),
            ("Sea", mask_sea), ("Foliage", mask_foliage), ("Highlights", mask_hi),
            ("Midtones", mask_mid), ("Shadows", mask_shadow)
        ]

        for sname, smask in segments:
            n_px = np.count_nonzero(smask)
            if n_px < 80:  # Skip if segment not present in this image
                continue
            
            sub_de = de_map[smask > 0]
            mean_de = np.mean(sub_de)
            p90_de = np.percentile(sub_de, 90)

            sub_ml = arr_ml[smask > 0].astype(float)
            sub_deh = arr_deh[smask > 0].astype(float)
            dr = np.mean(sub_ml[:, 0] - sub_deh[:, 0])
            dg = np.mean(sub_ml[:, 1] - sub_deh[:, 1])
            db = np.mean(sub_ml[:, 2] - sub_deh[:, 2])

            sub_l_ml = lab_ml[smask > 0, 0]
            sub_l_deh = lab_deh[smask > 0, 0]
            dL = np.mean(sub_l_ml - sub_l_deh)

            sub_a_ml = lab_ml[smask > 0, 1]; sub_b_ml = lab_ml[smask > 0, 2]
            sub_a_deh = lab_deh[smask > 0, 1]; sub_b_deh = lab_deh[smask > 0, 2]
            da = np.mean(sub_a_ml - sub_a_deh)
            db_lab = np.mean(sub_b_ml - sub_b_deh)

            segmented_metrics[sname].append({
                "frame": fname, "mean_de": mean_de, "p90_de": p90_de,
                "dL": dL, "da": da, "db": db_lab, "dRGB": (dr, dg, db), "n": n_px
            })

            diag = []
            if dL < -3.0: diag.append(f"Too Dark (dL={dL:.1f})")
            elif dL > +3.0: diag.append(f"Too Bright (dL={dL:.1f})")
            if da > +2.5: diag.append(f"Too Red/Magenta (da={da:.1f})")
            elif da < -2.5: diag.append(f"Too Green (da={da:.1f})")
            if db_lab > +2.5: diag.append(f"Too Yellow (db={db_lab:.1f})")
            elif db_lab < -2.5: diag.append(f"Too Blue (db={db_lab:.1f})")
            diag_str = " | ".join(diag) if diag else "Good Balance"

            print(f"  {sname:11s} ({n_px:5d} px): Mean dE00 = {mean_de:4.2f} (90th: {p90_de:4.2f}) | dL={dL:+5.1f} | dRGB=({dr:+4.1f},{dg:+4.1f},{db:+4.1f}) -> {diag_str}")

    print("\n" + "=" * 80)
    print("   OVERALL SEGMENTED SUMMARY ACROSS 10-IMAGE DATASET")
    print("=" * 80)
    for sname, list_m in segmented_metrics.items():
        if not list_m: continue
        all_des = [m["mean_de"] for m in list_m]
        all_dls = [m["dL"] for m in list_m]
        all_das = [m["da"] for m in list_m]
        all_dbs = [m["db"] for m in list_m]
        print(f"  {sname:11s} (across {len(list_m)} images): Average Mean dE00 = {np.mean(all_des):4.2f} | Average dL = {np.mean(all_dls):+5.1f} | da={np.mean(all_das):+4.1f} | db={np.mean(all_dbs):+4.1f}")

if __name__ == "__main__":
    main()

