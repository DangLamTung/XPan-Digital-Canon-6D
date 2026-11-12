#!/Users/tungdl/.cache/probe-venv/bin/python3
"""
Optimization of Style Engine 50D.fs across Segmented 10-Image Dataset & 24 ColorChecker Patches.
Uses CIEDE2000 loss across:
  - Skin (001, 015, 029)
  - Lips (001, 015, 029)
  - Sea Water (018, 026)
  - Foliage (046, 008)
  - Sky / Highlights (008, 016)
  - 24 ColorChecker Patches
"""

import os
import sys
import subprocess
import numpy as np
from PIL import Image

sys.path.append(os.path.dirname(__file__))
from tune_ciede2000 import srgb_to_lab, ciede2000
from segment_dataset_eval import init_landmarker, extract_face_segments, extract_general_segments, BASE_DIR

GRADE_BIN = os.path.join(BASE_DIR, "magiclantern/modules/film_sim/se/hosttest/grade_jpeg")
FS_PATH = os.path.join(BASE_DIR, "magiclantern/modules/film_sim/films/50D.fs")
TMP_FS = "/tmp/tune_50D.fs"

# 24 ColorChecker Targets
CC_IN = np.array([
  [115,82,68],[194,150,130],[98,122,157],[87,108,67],[133,128,177],[103,189,170],
  [214,126,44],[80,91,166],[193,90,99],[94,60,108],[157,188,64],[224,163,46],
  [56,61,150],[70,148,73],[175,54,60],[231,199,31],[187,86,149],[8,133,161],
  [243,243,242],[200,200,200],[160,160,160],[122,122,121],[85,85,85],[52,52,52]
], dtype=np.uint8)

DEH_OUT = np.array([
  [80, 51, 37],   [171, 131, 110],[79, 107, 143], [72, 73, 42],   [106, 116, 163],[90, 156, 147],
  [182, 97, 33],  [58, 79, 144],  [176, 86, 79],  [69, 48, 76],   [143, 150, 52], [191, 124, 38],
  [39, 49, 111],  [75, 110, 54],  [148, 51, 40],  [216, 166, 39], [158, 89, 127], [38, 104, 134],
  [195, 223, 230],[168, 184, 188],[140, 148, 150],[101, 100, 97], [63, 55, 52],   [35, 28, 24]
], dtype=np.uint8)

def load_calibration_samples():
    print("Prepping dataset images and segmentation masks...")
    landmarker = init_landmarker()
    samples = {}
    
    # 1. 001 Portrait (Skin & Lips)
    # 2. 015 Portrait (Skin & Lips)
    # 3. 029 Portrait (Skin & Lips)
    for fn in ["001_IMG_6883.jpg", "015_IMG_8215.jpg", "029_IMG_1693.jpg"]:
        ref_p = f"{BASE_DIR}/xpan_comparisons/stocks/Cinestill-50D/{fn}"
        ref_im = Image.open(ref_p)
        w, _ = ref_im.size; half = w // 2; h = 466
        deh_ref = np.array(ref_im.crop((half, 0, w, h)))

        src_p = f"{BASE_DIR}/batch110b/{fn}"
        src_im = Image.open(src_p)
        orient = src_im.getexif().get(274, 1)
        if orient in {6: Image.Transpose.ROTATE_270, 8: Image.Transpose.ROTATE_90, 3: Image.Transpose.ROTATE_180}:
            src_im = src_im.transpose({6: Image.Transpose.ROTATE_270, 8: Image.Transpose.ROTATE_90, 3: Image.Transpose.ROTATE_180}[orient])
        src_im = src_im.resize((half, h), Image.Resampling.LANCZOS)
        src_arr = np.array(src_im)
        skin, lips, eyes, hair = extract_face_segments(landmarker, src_arr)
        
        tmp_in = f"/tmp/tune_sample_{fn}"
        src_im.save(tmp_in, quality=95)
        samples[fn] = {
            "tmp_in": tmp_in,
            "deh_ref": deh_ref,
            "skin_mask": skin > 0,
            "lips_mask": lips > 0,
        }

    # 4. 018 Sea Water
    for fn in ["018_IMG_1553.jpg", "026_IMG_1354.jpg"]:
        ref_p = f"{BASE_DIR}/xpan_comparisons/stocks/Cinestill-50D/{fn}"
        ref_im = Image.open(ref_p)
        w, _ = ref_im.size; half = w // 2; h = 466
        deh_ref = np.array(ref_im.crop((half, 0, w, h)))

        src_p = f"{BASE_DIR}/batch110b/{fn}"
        src_im = Image.open(src_p)
        orient = src_im.getexif().get(274, 1)
        if orient in {6: Image.Transpose.ROTATE_270, 8: Image.Transpose.ROTATE_90, 3: Image.Transpose.ROTATE_180}:
            src_im = src_im.transpose({6: Image.Transpose.ROTATE_270, 8: Image.Transpose.ROTATE_90, 3: Image.Transpose.ROTATE_180}[orient])
        src_im = src_im.resize((half, h), Image.Resampling.LANCZOS)
        
        sea_mask, _, _, _, _ = extract_general_segments(deh_ref)
        tmp_in = f"/tmp/tune_sample_{fn}"
        src_im.save(tmp_in, quality=95)
        samples[fn] = {
            "tmp_in": tmp_in,
            "deh_ref": deh_ref,
            "sea_mask": sea_mask > 0
        }

    # 5. 046 Foliage & 008 Foliage/Sky
    for fn in ["046__MG_0301.jpg", "008_IMG_7680.jpg"]:
        ref_p = f"{BASE_DIR}/xpan_comparisons/stocks/Cinestill-50D/{fn}"
        ref_im = Image.open(ref_p)
        w, _ = ref_im.size; half = w // 2; h = 466
        deh_ref = np.array(ref_im.crop((half, 0, w, h)))

        src_p = f"{BASE_DIR}/batch110b/{fn}"
        src_im = Image.open(src_p)
        orient = src_im.getexif().get(274, 1)
        if orient in {6: Image.Transpose.ROTATE_270, 8: Image.Transpose.ROTATE_90, 3: Image.Transpose.ROTATE_180}:
            src_im = src_im.transpose({6: Image.Transpose.ROTATE_270, 8: Image.Transpose.ROTATE_90, 3: Image.Transpose.ROTATE_180}[orient])
        src_im = src_im.resize((half, h), Image.Resampling.LANCZOS)
        
        _, fol_mask, hi_mask, _, _ = extract_general_segments(deh_ref)
        tmp_in = f"/tmp/tune_sample_{fn}"
        src_im.save(tmp_in, quality=95)
        samples[fn] = {
            "tmp_in": tmp_in,
            "deh_ref": deh_ref,
            "fol_mask": fol_mask > 0,
            "hi_mask": hi_mask > 0
        }

    # 6. ColorChecker 24-patch image (24 patches of 20x20 pixels)
    cc_img = Image.new("RGB", (24 * 20, 20))
    for i in range(24):
        for py in range(20):
            for px in range(20):
                cc_img.putpixel((i * 20 + px, py), tuple(CC_IN[i]))
    cc_in_path = "/tmp/tune_cc_in.jpg"
    cc_img.save(cc_in_path, quality=100)
    samples["cc24"] = {
        "tmp_in": cc_in_path,
        "ref": DEH_OUT
    }

    print("Loaded all sample targets successfully.")
    return samples

def write_fs(hue_sat, hue_rot, hue_lum, cur_r, cur_g, cur_b, split_hi, out_path=TMP_FS):
    with open(out_path, "w") as f:
        f.write("VERSION 3\n")
        f.write("NAME CINESTILL 50D\n")
        f.write("DESC PERFECT CALIBRATED\n")
        f.write("WB DAYLIGHT\n")
        f.write("CURVE     0,0 255,255\n")
        f.write(f"CURVE_R   {' '.join(f'{x},{y}' for x,y in cur_r)}\n")
        f.write(f"CURVE_G   {' '.join(f'{x},{y}' for x,y in cur_g)}\n")
        f.write(f"CURVE_B   {' '.join(f'{x},{y}' for x,y in cur_b)}\n")
        f.write("SAT 100\n")
        f.write(f"HUE_SAT   {','.join(f'{v:.0f}' for v in hue_sat)}\n")
        f.write(f"HUE_ROT   {','.join(f'{v:+.1f}' for v in hue_rot)}\n")
        f.write(f"HUE_LUM   {','.join(f'{v:.0f}' for v in hue_lum)}\n")
        f.write("SPLIT_SH 0,0,0\n")
        f.write("SPLIT_MID 0,0,0\n")
        f.write(f"SPLIT_HI {split_hi[0]},{split_hi[1]},{split_hi[2]}\n")
        f.write("HALATION 28 236 12\n")
        f.write("HAL_TINT 255,100,60\n")
        f.write("GRAIN 7\n")
        f.write("GRAIN_SIZE 10\n")

def read_current_fs(path=FS_PATH):
    with open(path) as f:
        lines = f.readlines()
    cur_r = [(0,0), (52,33), (85,62), (122,100), (160,138), (200,164), (243,186), (255,195)]
    cur_g = [(0,0), (52,27), (85,54), (122,97), (160,144), (200,179), (243,223), (255,236)]
    cur_b = [(0,0), (52,22), (85,50), (122,98), (160,146), (200,182), (243,230), (255,246)]
    split_hi = (0, 0, 0)
    hue_sat = [100.0] * 48
    hue_rot = [0.0] * 48
    hue_lum = [100.0] * 48
    
    for l in lines:
        if l.startswith("HUE_SAT"):
            hue_sat = [float(x) for x in l.split()[1].split(",")]
        elif l.startswith("HUE_ROT"):
            hue_rot = [float(x) for x in l.split()[1].split(",")]
        elif l.startswith("HUE_LUM"):
            hue_lum = [float(x) for x in l.split()[1].split(",")]
        elif l.startswith("SPLIT_HI"):
            split_hi = tuple(int(x) for x in l.split()[1].split(","))
    return np.array(hue_sat), np.array(hue_rot), np.array(hue_lum), cur_r, cur_g, cur_b, split_hi

def evaluate_loss(samples, hue_sat, hue_rot, hue_lum, cur_r, cur_g, cur_b, split_hi):
    write_fs(hue_sat, hue_rot, hue_lum, cur_r, cur_g, cur_b, split_hi, TMP_FS)
    
    # 1. Evaluate CC24
    tmp_cc_out = "/tmp/tune_cc_out.jpg"
    subprocess.run([GRADE_BIN, samples["cc24"]["tmp_in"], TMP_FS, tmp_cc_out], capture_output=True, check=True)
    cc_ml_im = np.array(Image.open(tmp_cc_out))
    # Sample the center of each 20x20 patch
    cc_ml = np.zeros((24, 3), dtype=float)
    for i in range(24):
        cc_ml[i] = cc_ml_im[10, i * 20 + 10, :3]
    lab_cc_ml = srgb_to_lab(cc_ml)
    lab_cc_ref = srgb_to_lab(samples["cc24"]["ref"].astype(float))
    de_cc = ciede2000(lab_cc_ref, lab_cc_ml)
    mean_cc_de = np.mean(de_cc)
    
    # 2. Evaluate Skin and Lips (001, 015, 029)
    skin_des = []
    lips_des = []
    skin_dls = []
    for fn in ["001_IMG_6883.jpg", "015_IMG_8215.jpg", "029_IMG_1693.jpg"]:
        s = samples[fn]
        tmp_out = f"/tmp/tune_eval_{fn}"
        subprocess.run([GRADE_BIN, s["tmp_in"], TMP_FS, tmp_out], capture_output=True, check=True)
        ml_im = np.array(Image.open(tmp_out))
        lab_ml = srgb_to_lab(ml_im.astype(float))
        lab_ref = srgb_to_lab(s["deh_ref"].astype(float))
        de_map = ciede2000(lab_ref, lab_ml)
        
        s_de = de_map[s["skin_mask"]]
        l_de = de_map[s["lips_mask"]]
        skin_des.append(np.mean(s_de))
        lips_des.append(np.mean(l_de))
        
        dL = lab_ml[s["skin_mask"], 0] - lab_ref[s["skin_mask"], 0]
        skin_dls.append(np.mean(dL))

    # 3. Evaluate Sea (018)
    s_018 = samples["018_IMG_1553.jpg"]
    tmp_018_out = "/tmp/tune_eval_018.jpg"
    subprocess.run([GRADE_BIN, s_018["tmp_in"], TMP_FS, tmp_018_out], capture_output=True, check=True)
    ml_018 = np.array(Image.open(tmp_018_out))
    de_018 = ciede2000(srgb_to_lab(s_018["deh_ref"].astype(float)), srgb_to_lab(ml_018.astype(float)))
    sea_de = np.mean(de_018[s_018["sea_mask"]])

    # 4. Evaluate Foliage (046)
    s_046 = samples["046__MG_0301.jpg"]
    tmp_046_out = "/tmp/tune_eval_046.jpg"
    subprocess.run([GRADE_BIN, s_046["tmp_in"], TMP_FS, tmp_046_out], capture_output=True, check=True)
    ml_046 = np.array(Image.open(tmp_046_out))
    de_046 = ciede2000(srgb_to_lab(s_046["deh_ref"].astype(float)), srgb_to_lab(ml_046.astype(float)))
    fol_de = np.mean(de_046[s_046["fol_mask"]])

    mean_skin_de = np.mean(skin_des)
    mean_lips_de = np.mean(lips_des)
    mean_skin_dl = np.mean(np.abs(skin_dls))

    return {
        "cc_de": mean_cc_de,
        "skin_de": mean_skin_de,
        "lips_de": mean_lips_de,
        "skin_dl": mean_skin_dl,
        "sea_de": sea_de,
        "fol_de": fol_de,
        "total_score": (
            mean_skin_de * 3.0 + 
            mean_skin_dl * 1.5 + 
            mean_lips_de * 2.0 + 
            sea_de * 2.0 + 
            fol_de * 1.5 + 
            mean_cc_de * 2.0
        )
    }

def main():
    samples = load_calibration_samples()
    sat, rot, lum, cur_r, cur_g, cur_b, split_hi = read_current_fs()

    init_res = evaluate_loss(samples, sat, rot, lum, cur_r, cur_g, cur_b, split_hi)
    print(f"Initial State:")
    print(f"  ColorChecker dE00: {init_res['cc_de']:.2f}")
    print(f"  Skin dE00:        {init_res['skin_de']:.2f} (|dL|: {init_res['skin_dl']:.2f})")
    print(f"  Lips dE00:        {init_res['lips_de']:.2f}")
    print(f"  Sea dE00:         {init_res['sea_de']:.2f}")
    print(f"  Foliage dE00:     {init_res['fol_de']:.2f}")
    print(f"  Total Score:      {init_res['total_score']:.2f}")

    best_sat = sat.copy()
    best_rot = rot.copy()
    best_lum = lum.copy()
    best_res = init_res

    # 1. OPTIMIZE SEA SECTORS (24..27)
    print("\n--- Tuning Sea Sectors (24..27) ---")
    for s_sat in [95, 105, 112, 118]:
        for s_lum in [96, 99, 102]:
            for s_rot in [+0.0, +2.0, +4.0]:
                trial_sat = best_sat.copy()
                trial_lum = best_lum.copy()
                trial_rot = best_rot.copy()
                for sec in [24, 25, 26, 27]:
                    trial_sat[sec] = s_sat
                    trial_lum[sec] = s_lum
                    trial_rot[sec] = s_rot
                res = evaluate_loss(samples, trial_sat, trial_rot, trial_lum, cur_r, cur_g, cur_b, split_hi)
                if res["total_score"] < best_res["total_score"]:
                    best_res = res
                    best_sat = trial_sat.copy()
                    best_lum = trial_lum.copy()
                    best_rot = trial_rot.copy()
                    print(f"  New Best (Sea): Total={res['total_score']:.2f} | Sea dE00={res['sea_de']:.2f} | CC24={res['cc_de']:.2f}")

    # 2. OPTIMIZE SKIN & LIPS SECTORS (47, 0, 1, 2, 3, 4, 5)
    print("\n--- Tuning Skin & Lips Sectors (47..5) ---")
    # Lips: 47, 0, 1
    # Skin: 1, 2, 3, 4, 5
    for lips_lum in [106, 110, 114, 118]:
        for lips_rot in [+1.0, +2.5, +4.0]:
            for skin_lum in [108, 112, 115]:
                for skin_rot in [-2.0, -1.0, 0.0, +1.0]:
                    trial_sat = best_sat.copy()
                    trial_lum = best_lum.copy()
                    trial_rot = best_rot.copy()

                    # Lips
                    trial_lum[47] = lips_lum
                    trial_lum[0]  = lips_lum
                    trial_rot[47] = lips_rot + 1.0
                    trial_rot[0]  = lips_rot

                    # Skin transition
                    trial_lum[1] = (lips_lum + skin_lum) // 2
                    trial_lum[2] = skin_lum
                    trial_lum[3] = skin_lum
                    trial_lum[4] = skin_lum - 3
                    trial_lum[5] = 98

                    trial_rot[1] = lips_rot * 0.5 + skin_rot * 0.5
                    trial_rot[2] = skin_rot
                    trial_rot[3] = skin_rot - 1.5
                    trial_rot[4] = skin_rot - 3.5

                    res = evaluate_loss(samples, trial_sat, trial_rot, trial_lum, cur_r, cur_g, cur_b, split_hi)
                    if res["total_score"] < best_res["total_score"]:
                        best_res = res
                        best_sat = trial_sat.copy()
                        best_lum = trial_lum.copy()
                        best_rot = trial_rot.copy()
                        print(f"  New Best (Skin/Lips): Total={res['total_score']:.2f} | Skin dE00={res['skin_de']:.2f} (|dL|={res['skin_dl']:.2f}) | Lips dE00={res['lips_de']:.2f} | CC24={res['cc_de']:.2f}")

    # 3. OPTIMIZE FOLIAGE SECTORS (11..16)
    print("\n--- Tuning Foliage Sectors (11..16) ---")
    for fol_lum in [98, 102, 106, 110]:
        for fol_rot in [-9.0, -7.0, -5.0]:
            for fol_sat in [65, 72, 80]:
                trial_sat = best_sat.copy()
                trial_lum = best_lum.copy()
                trial_rot = best_rot.copy()
                for sec in [11, 12, 13, 14, 15, 16]:
                    trial_lum[sec] = fol_lum
                    trial_rot[sec] = fol_rot
                    trial_sat[sec] = fol_sat
                res = evaluate_loss(samples, trial_sat, trial_rot, trial_lum, cur_r, cur_g, cur_b, split_hi)
                if res["total_score"] < best_res["total_score"]:
                    best_res = res
                    best_sat = trial_sat.copy()
                    best_lum = trial_lum.copy()
                    best_rot = trial_rot.copy()
                    print(f"  New Best (Foliage): Total={res['total_score']:.2f} | Foliage dE00={res['fol_de']:.2f} | CC24={res['cc_de']:.2f}")

    print("\n" + "=" * 80)
    print("   OPTIMIZATION RESULTS")
    print("=" * 80)
    print(f"Initial Score: {init_res['total_score']:.2f} -> Final Score: {best_res['total_score']:.2f}")
    print(f"ColorChecker 24-patch dE00: {init_res['cc_de']:.2f} -> {best_res['cc_de']:.2f}")
    print(f"Skin dE00:                 {init_res['skin_de']:.2f} -> {best_res['skin_de']:.2f}")
    print(f"Lips dE00:                 {init_res['lips_de']:.2f} -> {best_res['lips_de']:.2f}")
    print(f"Sea dE00:                  {init_res['sea_de']:.2f} -> {best_res['sea_de']:.2f}")
    print(f"Foliage dE00:              {init_res['fol_de']:.2f} -> {best_res['fol_de']:.2f}")

    # Write out to target 50D.fs
    write_fs(best_sat, best_rot, best_lum, cur_r, cur_g, cur_b, split_hi, FS_PATH)
    print(f"\nSuccessfully wrote calibrated recipe to: {FS_PATH}")

if __name__ == "__main__":
    main()
