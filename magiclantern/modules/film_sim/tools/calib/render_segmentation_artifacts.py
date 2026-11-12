#!/Users/tungdl/.cache/probe-venv/bin/python3
"""
Renders a 5-panel per row composite board demonstrating:
  1. Magic Lantern SE C Engine Output
  2. Dehancer Desktop 1.1.0 Ground Truth
  3. MediaPipe & Color Physics Segmentation Mask Overlay
  4. Luma Error Map (dY = Y_ml - Y_deh: Neutral=Exact, Red=Too Bright, Blue=Too Dark)
  5. CIEDE2000 Color Error Heatmap
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

def get_luma_heatmap(dy_map, max_dy=12.0):
    """
    Diverging colormap for Luma Error (dY = Y_ml - Y_deh):
      - dy == 0: Neutral Charcoal/Slate [35, 38, 42] (exact parity)
      - dy > 0 (Over-bright/highlight too much): Amber/Orange to Hot Red [255, 30, 30]
      - dy < 0 (Too dark): Cyan [0, 190, 220] to Deep Blue
    """
    v = np.clip(dy_map / max_dy, -1.0, 1.0)
    r = np.zeros_like(v)
    g = np.zeros_like(v)
    b = np.zeros_like(v)

    # Negative (ML darker than Dehancer)
    neg = v < 0
    t_neg = -v[neg]
    r[neg] = 35 * (1.0 - t_neg)
    g[neg] = 38 * (1.0 - t_neg) + 180.0 * t_neg
    b[neg] = 42 * (1.0 - t_neg) + 240.0 * t_neg

    # Positive (ML brighter than Dehancer / highlight too bright)
    pos = v >= 0
    t_pos = v[pos]
    r[pos] = 35 * (1.0 - t_pos) + 255.0 * t_pos
    g[pos] = 38 * (1.0 - t_pos) + 140.0 * t_pos * (1.0 - t_pos)
    b[pos] = 42 * (1.0 - t_pos)

    heat_arr = np.stack([r, g, b], axis=-1).astype(np.uint8)
    return Image.fromarray(heat_arr)

def render_segmentation_composite():
    print("\n--- Generating 5-Panel Segmentation & Luma Error Proof Artifact ---")
    landmarker = init_landmarker()

    cases = [
        ("001_IMG_6883.jpg", "Fair Dermis Street Portrait", "face"),
        ("015_IMG_8215.jpg", "Fine Detail Close-Up Portrait", "face"),
        ("029_IMG_1693.jpg", "Warm Dermis Low-Key Portrait", "face"),
        ("018_IMG_1553.jpg", "Coastline Marine Sea & Surf", "sea"),
        ("008_IMG_7680.jpg", "Daylight Sky & Landscape Highlights", "highlights"),
        ("046__MG_0301.jpg", "Dense Forest Chlorophyll Green", "foliage")
    ]

    panel_w = 340
    panel_h = 226
    row_h = panel_h + 46
    board_w = panel_w * 5
    board_h = row_h * len(cases) + 80

    board = Image.new("RGB", (board_w, board_h), (18, 20, 24))
    draw = ImageDraw.Draw(board)

    # Global Header
    draw.rectangle([0, 0, board_w, 65], fill=(12, 14, 16))
    draw.text((25, 14), "CINESTILL 50D: FULL LUMA MAPPING, SEGMENTATION & COLOR PARITY AUDIT", fill=(255, 215, 120))
    draw.text((25, 38), "5-Panel Visual Parity: ML Output | Dehancer Target | Mask | Luma Error Map (dY) | CIEDE2000 Color Error Heatmap", fill=(180, 185, 190))

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
        
        luma_ml = 0.299 * arr_ml[..., 0] + 0.587 * arr_ml[..., 1] + 0.114 * arr_ml[..., 2]
        luma_deh = 0.299 * arr_deh[..., 0] + 0.587 * arr_deh[..., 1] + 0.114 * arr_deh[..., 2]
        dy_map = luma_ml - luma_deh

        lab_ml = srgb_to_lab(arr_ml)
        lab_deh = srgb_to_lab(arr_deh)
        de_map = ciede2000(lab_deh, lab_ml)
        mean_de = np.mean(de_map)
        
        luma_heat_im = get_luma_heatmap(dy_map, max_dy=12.0)
        ciede_heat_im = get_ciede_heatmap(de_map, max_de=10.0)

        # Build Segmentation Mask Overlay
        src_for_seg = np.array(src_im.resize((panel_w, panel_h), Image.Resampling.LANCZOS))
        seg_overlay = src_for_seg.copy()

        metrics_str = ""
        if seg_type == "face":
            skin, lips, eyes, hair = extract_face_segments(landmarker, src_for_seg)
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
            skin_dy = np.mean(dy_map[skin > 0]) if np.count_nonzero(skin) > 0 else 0.0
            skin_db = np.mean(lab_ml[skin > 0, 2] - lab_deh[skin > 0, 2]) if np.count_nonzero(skin) > 0 else 0.0
            metrics_str = f"Skin: dE00={skin_de:.2f} | dY={skin_dy:+.1f} | db*={skin_db:+.1f} (Yellow: >0) | Frame dE00={mean_de:.2f}"
        elif seg_type == "sea":
            sea_mask, _, _, _, _ = extract_general_segments(np.array(deh_ref))
            alpha = 0.45
            for c_idx, val in [(0, 0), (1, 220), (2, 255)]:
                seg_overlay[sea_mask > 0, c_idx] = (1 - alpha) * seg_overlay[sea_mask > 0, c_idx] + alpha * val
            sea_de = np.mean(de_map[sea_mask > 0]) if np.count_nonzero(sea_mask) > 0 else 0.0
            sea_dy = np.mean(dy_map[sea_mask > 0]) if np.count_nonzero(sea_mask) > 0 else 0.0
            metrics_str = f"Sea: dE00={sea_de:.2f} | dY={sea_dy:+.1f} | Frame dE00={mean_de:.2f}"
        elif seg_type == "highlights":
            _, _, hi_mask, _, _ = extract_general_segments(np.array(deh_ref))
            alpha = 0.45
            for c_idx, val in [(0, 255), (1, 255), (2, 100)]:
                seg_overlay[hi_mask > 0, c_idx] = (1 - alpha) * seg_overlay[hi_mask > 0, c_idx] + alpha * val
            hi_de = np.mean(de_map[hi_mask > 0]) if np.count_nonzero(hi_mask) > 0 else 0.0
            hi_dy = np.mean(dy_map[hi_mask > 0]) if np.count_nonzero(hi_mask) > 0 else 0.0
            hi_db = np.mean(lab_ml[hi_mask > 0, 2] - lab_deh[hi_mask > 0, 2]) if np.count_nonzero(hi_mask) > 0 else 0.0
            metrics_str = f"Highlights: dE00={hi_de:.2f} | dY={hi_dy:+.1f} | db*={hi_db:+.1f} | Frame dE00={mean_de:.2f}"
        else: # foliage
            _, fol_mask, _, _, _ = extract_general_segments(np.array(deh_ref))
            alpha = 0.45
            for c_idx, val in [(0, 50), (1, 255), (2, 50)]:
                seg_overlay[fol_mask > 0, c_idx] = (1 - alpha) * seg_overlay[fol_mask > 0, c_idx] + alpha * val
            fol_de = np.mean(de_map[fol_mask > 0]) if np.count_nonzero(fol_mask) > 0 else 0.0
            fol_dy = np.mean(dy_map[fol_mask > 0]) if np.count_nonzero(fol_mask) > 0 else 0.0
            metrics_str = f"Foliage: dE00={fol_de:.2f} | dY={fol_dy:+.1f} | Frame dE00={mean_de:.2f}"

        seg_im = Image.fromarray(seg_overlay)

        # Row Header
        draw.rectangle([0, y_top, board_w, y_top + 28], fill=(24, 26, 32))
        draw.text((15, y_top + 6), f"Row {idx+1}: {fname} — {title}", fill=(240, 240, 240))
        draw.text((panel_w * 2, y_top + 6), metrics_str, fill=(130, 255, 170))

        # Panel 1: ML Output
        board.paste(ml_graded, (0, y_top + 28))
        draw.rectangle([8, y_top + 34, 190, y_top + 54], fill=(0, 0, 0))
        draw.text((12, y_top + 37), "1. Magic Lantern SE", fill=(255, 255, 255))

        # Panel 2: Dehancer Reference
        board.paste(deh_ref, (panel_w, y_top + 28))
        draw.rectangle([panel_w + 8, y_top + 34, panel_w + 190, y_top + 54], fill=(0, 0, 0))
        draw.text((panel_w + 12, y_top + 37), "2. Dehancer Ground Truth", fill=(255, 255, 255))

        # Panel 3: Mask Overlay
        board.paste(seg_im, (panel_w * 2, y_top + 28))
        draw.rectangle([panel_w * 2 + 8, y_top + 34, panel_w * 2 + 200, y_top + 54], fill=(0, 0, 0))
        draw.text((panel_w * 2 + 12, y_top + 37), "3. Segmentation Mask", fill=(255, 220, 80))

        # Panel 4: Luma Error Map (dY)
        board.paste(luma_heat_im, (panel_w * 3, y_top + 28))
        draw.rectangle([panel_w * 3 + 8, y_top + 34, panel_w * 3 + 260, y_top + 54], fill=(0, 0, 0))
        draw.text((panel_w * 3 + 12, y_top + 37), "4. Luma Error (Red=Bright, Blue=Dark)", fill=(255, 160, 100))

        # Panel 5: CIEDE2000 Heatmap
        board.paste(ciede_heat_im, (panel_w * 4, y_top + 28))
        draw.rectangle([panel_w * 4 + 8, y_top + 34, panel_w * 4 + 230, y_top + 54], fill=(0, 0, 0))
        draw.text((panel_w * 4 + 12, y_top + 37), "5. CIEDE2000 (Navy=0, Cyan=3)", fill=(120, 220, 255))

    out_path = os.path.join(ARTIFACT_DIR, "segmentation_composite_evaluation_proof.jpg")
    board.save(out_path, quality=92)
    print(f"Artifact created: {out_path}")

if __name__ == "__main__":
    render_segmentation_composite()
