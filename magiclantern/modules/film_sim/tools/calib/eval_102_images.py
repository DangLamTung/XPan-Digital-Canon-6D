#!/usr/bin/env python3
"""
Full 102-Image Evaluation & Segment-Aware Diagnostic Benchmark for CineStill 50D.
Executes Magic Lantern SE hardware C engine against Dehancer Desktop 1.1.0 ground truth
across all 102 production test frames in xpan_comparisons/stocks/Cinestill-50D.
"""

import os
import sys
import time
import json
import subprocess
import numpy as np
import cv2
from PIL import Image

import mediapipe as mp
from mediapipe.tasks import python
from mediapipe.tasks.python import vision

BASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../../../.."))
GRADE_BIN = os.path.join(BASE_DIR, "magiclantern/modules/film_sim/se/hosttest/grade_jpeg")
TARGET_FS = os.path.join(BASE_DIR, "magiclantern/modules/film_sim/films/50D.fs")
ARTIFACT_DIR = "/Users/tungdl/.gemini/antigravity/brain/b15af59f-a878-4ed2-99f5-5e2a2fe2371a"

sys.path.append(os.path.dirname(__file__))
from tune_ciede2000 import srgb_to_lab, ciede2000

DEH_DIR = os.path.join(BASE_DIR, "xpan_comparisons/stocks/Cinestill-50D")
SRC_DIR = os.path.join(BASE_DIR, "batch110b")

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
    mask_hair = np.zeros((h, w), dtype=np.uint8)

    if not res.face_landmarks:
        # Fallback dermal color filter if face detector doesn't trigger
        arr = img_rgb.astype(float)
        r, g, b = arr[..., 0], arr[..., 1], arr[..., 2]
        dermal = (r > 120) & (g > 80) & (b > 60) & (r > g + 10) & (g > b) & ((r - g) > (g - b)) & (r < 240)
        return dermal.astype(np.uint8) * 255, mask_lips, mask_hair

    lm = res.face_landmarks[0]
    def get_poly(indices):
        return np.array([[int(lm[i].x * w), int(lm[i].y * h)] for i in indices], dtype=np.int32)

    poly_face = get_poly(FACE_OVAL_INDEX)
    poly_lips = get_poly(LIPS_INDEX)
    poly_leye = get_poly(LEFT_EYE_INDEX)
    poly_reye = get_poly(RIGHT_EYE_INDEX)

    cv2.fillPoly(mask_lips, [poly_lips], 255)
    mask_eyes = np.zeros((h, w), dtype=np.uint8)
    cv2.fillPoly(mask_eyes, [poly_leye, poly_reye], 255)

    face_full = np.zeros((h, w), dtype=np.uint8)
    cv2.fillPoly(face_full, [poly_face], 255)

    mask_skin = cv2.bitwise_and(face_full, cv2.bitwise_not(mask_lips))
    mask_skin = cv2.bitwise_and(mask_skin, cv2.bitwise_not(mask_eyes))

    min_x, max_x = np.min(poly_face[:, 0]), np.max(poly_face[:, 0])
    min_y, max_y = np.min(poly_face[:, 1]), np.max(poly_face[:, 1])
    hair_top = max(0, min_y - int((max_y - min_y) * 0.45))
    hair_mask_candidate = np.zeros((h, w), dtype=np.uint8)
    hair_mask_candidate[hair_top:min_y + 20, max(0, min_x - 30):min(w, max_x + 30)] = 255
    luma = 0.299 * img_rgb[..., 0] + 0.587 * img_rgb[..., 1] + 0.114 * img_rgb[..., 2]
    mask_hair = cv2.bitwise_and(hair_mask_candidate, (luma < 120).astype(np.uint8) * 255)
    mask_hair = cv2.bitwise_and(mask_hair, cv2.bitwise_not(face_full))

    return mask_skin, mask_lips, mask_hair

def extract_general_segments(img_rgb):
    arr = img_rgb.astype(float)
    r, g, b = arr[..., 0], arr[..., 1], arr[..., 2]
    luma = 0.299 * r + 0.587 * g + 0.114 * b

    # Sea / Oceanic Surf
    mask_sea = ((b > r + 15) & (g > r + 5) & (luma > 30) & (luma < 220)).astype(np.uint8) * 255
    # Foliage Green
    mask_foliage = ((g > r + 10) & (g > b + 10) & (luma > 25) & (luma < 180)).astype(np.uint8) * 255
    # Highlights
    mask_hi = ((luma > 195) | (np.maximum(np.maximum(r, g), b) > 225)).astype(np.uint8) * 255
    # Shadows
    mask_shadow = (luma < 50).astype(np.uint8) * 255
    # Midtones
    mask_mid = ((luma >= 70) & (luma <= 170) & (mask_sea == 0) & (mask_foliage == 0)).astype(np.uint8) * 255

    return mask_sea, mask_foliage, mask_hi, mask_shadow, mask_mid

def evaluate_all_102():
    files = sorted(os.listdir(DEH_DIR))
    print(f"Beginning 102-Frame Evaluation on CineStill 50D...")
    print(f"Target Recipe: {TARGET_FS}")
    print(f"Total Frames: {len(files)}")
    print("=" * 80)

    landmarker = init_landmarker()

    frame_results = []
    segment_records = {
        "Skin": [], "Lips": [], "Hair": [],
        "Sea": [], "Foliage": [], "Highlights": [],
        "Midtones": [], "Shadows": []
    }

    t0 = time.time()
    for idx, fname in enumerate(files):
        ref_p = os.path.join(DEH_DIR, fname)
        src_p = os.path.join(SRC_DIR, fname)
        if not os.path.exists(src_p):
            print(f"  Missing source for {fname}, skipping")
            continue

        ref_im = Image.open(ref_p)
        w, _ = ref_im.size
        half = w // 2
        h = 466
        deh_crop = ref_im.crop((half, 0, w, h))
        arr_deh = np.array(deh_crop, dtype=float)

        src_im = Image.open(src_p)
        orient = src_im.getexif().get(274, 1)
        if orient in {6: Image.Transpose.ROTATE_270, 8: Image.Transpose.ROTATE_90, 3: Image.Transpose.ROTATE_180}:
            src_im = src_im.transpose({6: Image.Transpose.ROTATE_270, 8: Image.Transpose.ROTATE_90, 3: Image.Transpose.ROTATE_180}[orient])
        src_resized = src_im.resize((half, h), Image.Resampling.LANCZOS)

        tmp_in = f"/tmp/eval102_{fname}_s.jpg"
        tmp_out = f"/tmp/eval102_{fname}_m.jpg"
        src_resized.save(tmp_in, quality=98)

        subprocess.run([GRADE_BIN, tmp_in, TARGET_FS, tmp_out], capture_output=True, check=True)
        ml_im = Image.open(tmp_out)
        arr_ml = np.array(ml_im, dtype=float)

        lab_deh = srgb_to_lab(arr_deh)
        lab_ml = srgb_to_lab(arr_ml)
        de_map = ciede2000(lab_deh, lab_ml)

        mean_de = float(np.mean(de_map))
        med_de = float(np.median(de_map))
        p90_de = float(np.percentile(de_map, 90))
        max_de = float(np.max(de_map))
        std_de = float(np.std(de_map))

        # Mean RGB diff
        dr = float(np.mean(arr_ml[..., 0] - arr_deh[..., 0]))
        dg = float(np.mean(arr_ml[..., 1] - arr_deh[..., 1]))
        db = float(np.mean(arr_ml[..., 2] - arr_deh[..., 2]))

        # Segments
        mask_skin, mask_lips, mask_hair = extract_face_segments(landmarker, np.array(src_resized))
        mask_sea, mask_foliage, mask_hi, mask_shadow, mask_mid = extract_general_segments(arr_deh)

        segs = [
            ("Skin", mask_skin), ("Lips", mask_lips), ("Hair", mask_hair),
            ("Sea", mask_sea), ("Foliage", mask_foliage), ("Highlights", mask_hi),
            ("Midtones", mask_mid), ("Shadows", mask_shadow)
        ]

        frame_seg_info = {}
        for sname, smask in segs:
            n_px = int(np.count_nonzero(smask))
            if n_px >= 60:
                s_de = float(np.mean(de_map[smask > 0]))
                s_l = float(np.mean(lab_ml[smask > 0, 0] - lab_deh[smask > 0, 0]))
                s_a = float(np.mean(lab_ml[smask > 0, 1] - lab_deh[smask > 0, 1]))
                s_b = float(np.mean(lab_ml[smask > 0, 2] - lab_deh[smask > 0, 2]))
                s_dr = float(np.mean(arr_ml[smask > 0, 0] - arr_deh[smask > 0, 0]))
                s_dg = float(np.mean(arr_ml[smask > 0, 1] - arr_deh[smask > 0, 1]))
                s_db = float(np.mean(arr_ml[smask > 0, 2] - arr_deh[smask > 0, 2]))
                record = {
                    "frame": fname, "mean_de": s_de, "n": n_px,
                    "dL": s_l, "da": s_a, "db": s_b, "dRGB": (s_dr, s_dg, s_db)
                }
                segment_records[sname].append(record)
                frame_seg_info[sname] = s_de

        frame_results.append({
            "idx": idx + 1,
            "fname": fname,
            "mean_de": mean_de,
            "med_de": med_de,
            "p90_de": p90_de,
            "max_de": max_de,
            "std_de": std_de,
            "dRGB": (dr, dg, db),
            "segs": frame_seg_info
        })

        if (idx + 1) % 10 == 0 or (idx + 1) == len(files):
            print(f"  Processed {idx + 1:3d}/{len(files)} frames ({time.time() - t0:.1f}s) | Current Frame Mean dE00: {mean_de:.2f}")

    total_time = time.time() - t0
    print("=" * 80)
    print(f"Completed evaluation of {len(frame_results)} frames in {total_time:.2f}s ({total_time / len(frame_results):.2f}s / frame)")

    # Global Corpus Statistics
    all_means = [f["mean_de"] for f in frame_results]
    all_medians = [f["med_de"] for f in frame_results]
    all_p90s = [f["p90_de"] for f in frame_results]

    corpus_mean = float(np.mean(all_means))
    corpus_median = float(np.median(all_medians))
    corpus_p90 = float(np.mean(all_p90s))
    corpus_min = float(np.min(all_means))
    corpus_max = float(np.max(all_means))

    # Distribution Bins
    b_under_3 = sum(1 for m in all_means if m <= 3.0)
    b_3_to_4 = sum(1 for m in all_means if 3.0 < m <= 4.0)
    b_4_to_5 = sum(1 for m in all_means if 4.0 < m <= 5.0)
    b_5_to_6 = sum(1 for m in all_means if 5.0 < m <= 6.0)
    b_above_6 = sum(1 for m in all_means if m > 6.0)

    # Sort best and worst
    sorted_by_de = sorted(frame_results, key=lambda x: x["mean_de"])
    top_10_best = sorted_by_de[:10]
    top_10_worst = sorted_by_de[-10:]

    summary_data = {
        "total_frames": len(frame_results),
        "corpus_mean_de": corpus_mean,
        "corpus_median_de": corpus_median,
        "corpus_p90_de": corpus_p90,
        "corpus_min_de": corpus_min,
        "corpus_max_de": corpus_max,
        "bins": {
            "under_3": (b_under_3, b_under_3 / len(all_means) * 100),
            "3_to_4": (b_3_to_4, b_3_to_4 / len(all_means) * 100),
            "4_to_5": (b_4_to_5, b_4_to_5 / len(all_means) * 100),
            "5_to_6": (b_5_to_6, b_5_to_6 / len(all_means) * 100),
            "above_6": (b_above_6, b_above_6 / len(all_means) * 100),
        },
        "segment_summary": {},
        "top_10_best": top_10_best,
        "top_10_worst": top_10_worst,
        "all_frames": frame_results
    }

    print("\n--- SEGMENT-AWARE CORPUS SUMMARY ---")
    for sname, recs in segment_records.items():
        if recs:
            s_means = [r["mean_de"] for r in recs]
            s_dls = [r["dL"] for r in recs]
            s_das = [r["da"] for r in recs]
            s_dbs = [r["db"] for r in recs]
            s_dr = [r["dRGB"][0] for r in recs]
            s_dg = [r["dRGB"][1] for r in recs]
            s_db = [r["dRGB"][2] for r in recs]
            summary_data["segment_summary"][sname] = {
                "count": len(recs),
                "mean_de": float(np.mean(s_means)),
                "dL": float(np.mean(s_dls)),
                "da": float(np.mean(s_das)),
                "db": float(np.mean(s_dbs)),
                "dRGB": (float(np.mean(s_dr)), float(np.mean(s_dg)), float(np.mean(s_db)))
            }
            print(f"  {sname:11s} ({len(recs):3d} frames): Mean dE00 = {np.mean(s_means):.2f} | dL={np.mean(s_dls):+4.1f} | da={np.mean(s_das):+4.1f} | db={np.mean(s_dbs):+4.1f}")

    # Save JSON summary
    json_path = os.path.join(ARTIFACT_DIR, "eval_102_summary.json")
    with open(json_path, "w") as f:
        json.dump(summary_data, f, indent=2)
    print(f"\nJSON summary saved to {json_path}")

    return summary_data

if __name__ == "__main__":
    evaluate_all_102()
