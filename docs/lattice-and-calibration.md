# Lưới màu và hiệu chuẩn stock — bản đúng

Tài liệu này thay cho những chỗ sai trong `docs/film-stock-test-notes.md`. Mọi con số
dưới đây **tính trực tiếp từ** `renders/stocks/*/metrics.json` (trường `per_frame`),
không chép từ văn bản nào.

## 1. Lưới màu thực tế: 12 × 17 × 17

```c
/* magiclantern/modules/film_sim/se/engine/look.h */
#define SE_YUV3_NY 12      /* nodes along luma        */
#define SE_YUV3_NC 17      /* nodes along Cb, and Cr  */

/* magiclantern/modules/film_sim/fs_core.h  (phải khớp) */
#define FS_UV3_NY 12
#define FS_UV3_NC 17
```

Nên lưới là **12 (Y) × 17 (Cb) × 17 (Cr)** = 3468 nút, không phải "17×16×16" như bản
ghi chú cũ. Có một kiểm tra lúc biên dịch (`fs_lv3d_shape_check`) để hai bên không
lệch nhau.

## 2. Cách đo

- Render bằng chính engine của module: `se/hosttest/grade_jpeg` (đường develop RGB).
- So với ảnh tham chiếu Dehancer của cùng khung, khoảng cách **CIEDE2000** trên mọi
  pixel, lấy **trung vị mỗi khung**, rồi lấy trung vị/trung bình qua bộ ảnh.
- Bộ ảnh: **101 khung** (`batch110b/` ghép với corpus Dehancer). Con số "102" trong
  ghi chú cũ là sai; chỉ riêng Superia 200 dùng corpus `gt2048` có 102 khung.

Lệnh tái lập:

```sh
./tools/eval_gt_stock.py --stock "Superia 200" --fs SUP200 \
    --gt gt2048/Fujicolor_Superia_200_+_3513/images --out renders/stocks/Superia-200
```

Mỗi lần chạy ghi `metrics.json` gồm `per_frame` — đó là nguồn của bảng dưới.

## 3. Kết quả tốt nhất cho mỗi stock

Một stock có thể có nhiều lần chạy (các vòng tune). Bảng lấy **lần tốt nhất** theo
trung vị. Cột "frames" là số khung thật đã đo.

| Stock | frames | mean ΔE00 | median ΔE00 | ≤5.0 | ≤6.0 |
|---|---|---|---|---|---|
| XP2 Super | 101 | 1.12 | **0.99** | 100% | 100% |
| acros | 101 | 2.10 | **2.17** | 100% | 100% |
| Acros 100 | 101 | 2.10 | **2.18** | 100% | 100% |
| Provia 100F | 101 | 2.65 | **2.57** | 100% | 100% |
| Gold 200 | 101 | 3.10 | **3.07** | 99% | 100% |
| P160VC | 101 | 3.23 | **3.10** | 95% | 99% |
| Portra 160VC | 101 | 3.23 | **3.10** | 95% | 99% |
| gold | 101 | 3.19 | **3.16** | 99% | 100% |
| HP5 | 101 | 3.23 | **3.21** | 99% | 100% |
| HP5 Plus | 101 | 3.22 | **3.21** | 99% | 100% |
| CP200 | 101 | 3.20 | **3.21** | 99% | 100% |
| ColorPlus 200 | 101 | 3.18 | **3.22** | 99% | 100% |
| Portra 800 | 101 | 3.33 | **3.25** | 100% | 100% |
| Portra 800 (cal v1) | 101 | 3.33 | **3.25** | 100% | 100% |
| P800 | 101 | 3.34 | **3.25** | 100% | 100% |
| Ektachrome E100 | 101 | 3.28 | **3.32** | 99% | 100% |
| Superia 200 | 102 | 3.49 | **3.33** | 96% | 99% |
| Portra 400 | 101 | 3.35 | **3.36** | 98% | 100% |
| portra | 101 | 3.36 | **3.36** | 98% | 100% |
| V100 | 101 | 3.49 | **3.39** | 94% | 98% |
| Velvia 100 | 101 | 3.49 | **3.39** | 94% | 98% |
| Provia 100F (cal v1) | 101 | 3.62 | **3.53** | 95% | 100% |
| P160NC | 101 | 3.61 | **3.56** | 92% | 99% |
| Portra 160NC | 101 | 3.61 | **3.56** | 92% | 99% |
| Cinestill 50D | 101 | 3.70 | **3.57** | 91% | 98% |
| 800T | 101 | 3.76 | **3.74** | 98% | 100% |
| CineStill 800T | 101 | 3.76 | **3.75** | 98% | 99% |
| Ektar 100 | 101 | 3.88 | **3.76** | 85% | 96% |
| Pro 400H | 101 | 3.76 | **3.82** | 99% | 100% |
| 400H | 101 | 3.77 | **3.83** | 99% | 100% |
| Portra 160 | 101 | 3.96 | **3.90** | 86% | 97% |
| Velvia 50 | 101 | 4.16 | **3.99** | 75% | 92% |
| velvia | 101 | 4.20 | **4.06** | 75% | 92% |
| ColorPlus 200 (cal v1) | 101 | 4.48 | **4.40** | 78% | 97% |
| Portra 400 (cal v1) | 101 | 4.70 | **4.48** | 69% | 87% |
| Velvia 50 (grp) | 101 | 4.70 | **4.49** | 63% | 79% |
| Velvia 50 (mid) | 101 | 4.82 | **4.55** | 59% | 76% |
| Velvia 50 (rich) | 101 | 5.02 | **4.69** | 55% | 72% |
| Kodachrome 64 | 101 | 4.72 | **4.80** | 65% | 96% |
| Pro 400H (cal v1) | 101 | 6.16 | **5.11** | 48% | 65% |
| Gold 200 (cal v1) | 101 | 5.35 | **5.30** | 38% | 79% |
| Velvia 50 (cal v1) | 101 | 6.01 | **5.60** | 30% | 57% |
| Ektar 100 (cal v1) | 101 | 6.21 | **6.02** | 25% | 49% |
| Kodachrome 64 (cal2) | 101 | 6.12 | **6.16** | 4% | 39% |
| Kodachrome 64 (cal v1) | 101 | 10.83 | **8.86** | 6% | 14% |

## 4. Những chỗ sai trong `docs/film-stock-test-notes.md`

| điều ghi chú nói | thực tế | |
|---|---|---|
| chuẩn `SE2_HUE` | không tồn tại trong `.c/.h/.py/.fs` nào | ❌ bịa |
| lưới `17×16×16` | `12×17×17` | ❌ sai |
| bộ ảnh `102` | `101` (trừ Superia 200 dùng corpus riêng) | ❌ sai |
| 50D median `4.07`, mean `4.51` | **3.57 / 3.70** | ❌ sai |
| 50D `74.5%` đạt ≤5.0 | **91.1%** | ❌ sai |
| 50D `97.1%` đạt ≤6.0 | **98.0%** | ✅ gần đúng |
| MediaPipe segmentation | `tools/calib/models/selfie_multiclass_256x256.*` + `import mediapipe` trong `eval_102_images.py` | ✅ đúng |
| ΔE theo 8 vùng (da, môi, tóc, biển…) | script có chạy, nhưng **không lưu kết quả** ra file | ⚠️ không kiểm chứng được |

## 5. Điều cần làm để các số vùng trở nên kiểm chứng được

`eval_102_images.py` in bảng tổng kết ra màn hình rồi thôi. Chỉ cần ghi nó ra
`renders/stocks/<stock>/segments.json` là mọi con số vùng (da/môi/tóc/lá/biển/
shadow/mid/highlight) trở thành dữ liệu đối chiếu được, giống `metrics.json`.
