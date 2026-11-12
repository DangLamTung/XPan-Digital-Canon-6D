#!/Users/tungdl/.cache/probe-venv/bin/python3
"""
Renders a 4-row composite board demonstrating MediaPipe Face Landmarking & Color Physics Segmentation
together with the calibrated CIEDE2000 error heatmaps and side-by-side ML vs Dehancer comparisons.
"""

import os
import sys
import subprocess
import numpy as np
import cv2
from PIL import Image, ImageDraw, ImageFont

BASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../../../.."))
GRADE_BIN = os.path.join(BASE_DIR, "magiclantern/modules/film_sim/se/hosttest/grade_jpeg")
TARGET_FS = os.path.join(BASE_DIR, "magiclantern/modules/film_sim/films/50D.fs")
ARTIFACT_DIR = "/Users/tungdl/.gemini/antigravity/brain/b15af59f-a878-4ed2-99f5-5e2a2fe2371a"

sys.path.append(os.path.dirname(__file__))
from tune_ciede2000 import srgb_to_lab, ciede2000
from render_calib_artifacts import get_ciede_heatmap
from segment_dataset_eval import init_landmarker, extract_face_segments, extract_general_segments

def render_segmentation_composite():
    print("\n--- Generating Segmentation Composite Evaluation Proof Artifact ---")
    landmarker = init_landmarker()

    cases = [
        ("001_IMG_6883.jpg", "Fair Dermis Portrait", "face"),
        ("015_IMG_8215.jpg", "Fine Detail Close-Up Portrait", "face"),
        ("018_IMG_1553.jpg", "Coastline Marine Sea & Surf", "sea"),
        ("046__MG_0301.jpg", "Dense Forest Chlorophyll Green", "foliage")
    ]

    panel_w = 400
    panel_h = 266
    row_h = panel_h + 50
    board_w = panel_w * 4
    board_h = row_h * len(cases) + 80

    board = Image.new("RGB", (board_w, board_h), (20, 22, 26))
    draw = ImageDraw.Draw(board)

    # Global Header
    draw.rectangle([0, 0, board_w, 65], fill=(14, 16, 18))
    draw.text((25, 14), "CINESTILL 50D: ML-SEGMENTED DATASET EVALUATION & CIEDE2000 ERROR ANALYSIS", fill=(255, 215, 120))
    draw.text((25, 38), "MediaPipe Face Landmark (Skin, Lips, Eyes, Hair) + Color Physics (Sea, Foliage) | Objective ISO Parity vs Dehancer 1.1.0", fill=(180, 185, 190))

    for idx, (fname, title, seg_type) in enumerate(cases):
        y_top = 70 + idx * row_h
        ref_p = f"{BASE_DIR}/xpan_comparisons/stocks/Cinestill-50D/{fname}"
        ref_im = Image.open(ref_p)
        w, _ = ref_im.size
        half = w // 2
        h = 466
        deh_ref = ref_im.crop((half, 0, w, h)).resize((panel_w, panel_h), Image.Resampling.LANCZOS)

        src_p = f"{BASE_DIR}/batch110b/{fname}"
        src_im = Image.open(src_p)
        orient = src_im.getexif().get(274, 1)
        if orient in {6: Image.Transpose.ROTATE_270, 8: Image.Transpose.ROTATE_90, 3: Image.Transpose.ROTATE_180}:
            src_im = src_im.transpose({6: Image.Transpose.ROTATE_270, 8: Image.Transpose.ROTATE_90, 3: Image.Transpose.ROTATE_180}[orient])
        src_resized = src_im.resize((half, h), Image.Resampling.LANCZOS)
        
        tmp_in = f"/tmp/seg_src_{fname}"
        tmp_out = f"/tmp/seg_out_{fname}"
        src_resized.save(tmp_in, quality=95)
        subprocess.run([GRADE_BIN, tmp_in, TARGET_FS, tmp_out], capture_output=True, check=True)
        ml_graded = Image.open(tmp_out).convert("RGB").resize((panel_w, panel_h), Image.Resampling.LANCZOS)

        arr_ml = np.array(ml_graded, dtype=float)
        arr_deh = np.array(deh_ref, dtype=float)
        lab_ml = srgb_to_lab(arr_ml)
        lab_deh = srgb_to_lab(arr_deh)
        de_map = ciede2000(lab_deh, lab_ml)
        mean_de = np.mean(de_map)
        heatmap_im = get_ciede_heatmap(de_map, max_de=10.0)

        # Build Segmentation Mask Overlay
        src_for_seg = np.array(src_im.resize((panel_w, panel_h), Image.Resampling.LANCZOS))
        seg_overlay = src_for_seg.copy()

        metrics_str = ""
        if seg_type == "face":
            skin, lips, eyes, hair = extract_face_segments(landmarker, src_for_seg)
            # Overlay colors (RGBA blend)
            # Skin: Peach/Orange (255, 170, 80)
            # Lips: Carmine Red (255, 30, 60)
            # Eyes: Cyan Blue (0, 200, 255)
            # Hair: Violet (180, 70, 220)
            alpha = 0.45
            for c_idx, val in [(0, 255), (1, 170), (2, 80)]:
                seg_overlay[skin > 0, c_idx] = (1 - alpha) * seg_overlay[skin > 0, c_idx] + alpha * val
            for c_idx, val in [(0, 255), (1, 30), (2, 60)]:
                seg_overlay[lips > 0, c_idx] = (1 - alpha) * seg_overlay[lips > 0, c_idx] + alpha * val
            for c_idx, val in [(0, 0), (1, 200), (2, 255)]:
                seg_overlay[eyes > 0, c_idx] = (1 - alpha) * seg_overlay[eyes > 0, c_idx] + alpha * val
            for c_idx, val in [(0, 180), (1, 70), (2, 220)]:
                seg_overlay[hair > 0, c_idx] = (1 - alpha) * seg_overlay[hair > 0, c_idx] + alpha * val

            skin_de = np.mean(de_map[skin > 0]) if np.count_nonzero(skin) > 0 else 0.0
            lips_de = np.mean(de_map[lips > 0]) if np.count_nonzero(lips) > 0 else 0.0
            skin_dl = np.mean(lab_ml[skin > 0, 0] - lab_deh[skin > 0, 0]) if np.count_nonzero(skin) > 0 else 0.0
            metrics_str = f"Skin dE00: {skin_de:.2f} (dL: {skin_dl:+.1f}) | Lips dE00: {lips_de:.2f} | Overall: {mean_de:.2f}"
        elif seg_type == "sea":
            sea_mask, _, _, _, _ = extract_general_segments(np.array(deh_ref))
            alpha = 0.45
            for c_idx, val in [(0, 0), (1, 220), (2, 255)]:
                seg_overlay[sea_mask > 0, c_idx] = (1 - alpha) * seg_overlay[sea_mask > 0, c_idx] + alpha * val
            sea_de = np.mean(de_map[sea_mask > 0]) if np.count_nonzero(sea_mask) > 0 else 0.0
            metrics_str = f"Oceanic Sea dE00: {sea_de:.2f} | Overall Frame dE00: {mean_de:.2f}"
        else: # foliage
            _, fol_mask, _, _, _ = extract_general_segments(np.array(deh_ref))
            alpha = 0.45
            for c_idx, val in [(0, 50), (1, 255), (2, 50)]:
                seg_overlay[fol_mask > 0, c_idx] = (1 - alpha) * seg_overlay[fol_mask > 0, c_idx] + alpha * val
            fol_de = np.mean(de_map[fol_mask > 0]) if np.count_nonzero(fol_mask) > 0 else 0.0
            metrics_str = f"Chlorophyll Foliage dE00: {fol_de:.2f} | Overall Frame dE00: {mean_de:.2f}"

        seg_im = Image.fromarray(seg_overlay)

        # Row Header
        draw.rectangle([0, y_top, board_w, y_top + 32], fill=(28, 30, 36))
        draw.text((20, y_top + 8), f"Row {idx+1}: {fname} — {title}", fill=(240, 240, 240))
        draw.text((panel_w * 2, y_top + 8), metrics_str, fill=(130, 255, 170))

        # Panel 1: ML Graded
        board.paste(ml_graded, (0, y_top + 32))
        draw.rectangle([10, y_top + 38, 220, y_top + 60], fill=(0, 0, 0))
        draw.text((16, y_top + 42), "Magic Lantern SE C Engine", fill=(255, 255, 255))

        # Panel 2: Dehancer Reference
        board.paste(deh_ref, (panel_w, y_top + 32))
        draw.rectangle([panel_w + 10, y_top + 38, panel_w + 220, y_top + 60], fill=(0, 0, 0))
        draw.text((panel_w + 16, y_top + 42), "Dehancer Desktop 1.1.0", fill=(255, 255, 255))

        # Panel 3: Segmentation Mask Overlay
        board.paste(seg_im, (panel_w * 2, y_top + 32))
        draw.rectangle([panel_w * 2 + 10, y_top + 38, panel_w * 2 + 250, y_top + 60], fill=(0, 0, 0))
        draw.text((panel_w * 2 + 16, y_top + 42), "MediaPipe & Physics Mask", fill=(255, 220, 80))

        # Panel 4: CIEDE2000 Error Heatmap
        board.paste(heatmap_im, (panel_w * 3, y_top + 32))
        draw.rectangle([panel_w * 3 + 10, y_top + 38, panel_w * 3 + 250, y_top + 60], fill=(0, 0, 0))
        draw.text((panel_w * 3 + 16, y_top + 42), "CIEDE2000 Error Heatmap", fill=(120, 220, 255))

    out_path = os.path.join(ARTIFACT_DIR, "segmentation_composite_evaluation_proof.jpg")
    board.save(out_path, quality=92)
    print(f"Artifact created: {out_path}")

if __name__ == "__main__":
    render_segmentation_composite()
