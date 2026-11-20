# Ghi chú Kiểm thử & Hiệu chuẩn Film Stock (`film_sim`)

Tài liệu tổng hợp danh sách các Film Stock đã được kiểm thử, phương pháp kiểm thử (Host C Engine & On-Camera Canon 6D 1.1.6), và các vùng màu phân đoạn (Skin tone, Foliage, Sea, Highlights/Midtones/Shadows) dùng để đo lường.

---

## 1. Danh sách các Stock đã Test & Mức độ Kiểm thử

### 1.1. Nhóm đã hiệu chuẩn 48-Sector (`VERSION 3` / `SE2_HUE`) & Benchmark 102 ảnh Dehancer
| Stock | File Recipe | Phương pháp đã chạy | Kết quả đo lường |
| :--- | :--- | :--- | :--- |
| **CineStill 50D** | `magiclantern/modules/film_sim/films/50D.fs` | 1. Tối ưu hóa CIEDE2000 trên canvas đa phân đoạn + ColorChecker 24/96 patch<br>2. Chạy full benchmark **102 ảnh thực tế** so với Dehancer Desktop 1.1.0<br>3. Chụp thực tế trên thân máy Canon 6D (tỉ lệ 3:2 và 65:24 XPan) | • **Mean $\Delta E_{00}$ (102 ảnh):** `4.51`<br>• **Median $\Delta E_{00}$:** `4.07`<br>• **Tỉ lệ đạt $\Delta E_{00} \le 5.0$:** `74.5%` (76/102 ảnh)<br>• **Tỉ lệ đạt $\Delta E_{00} \le 6.0$:** `97.1%` (99/102 ảnh) |
| **CineStill 800T** | `magiclantern/modules/film_sim/films/800T.fs` (`soup800.fs`) | Đã có bản công thức `VERSION 3` (48-sector) và đã test chọn/bake trên thân máy 6D | *(Lưu ý: `fs_stock_alias` trong `film_sim.c` hiện vẫn đang trỏ `CineStill 800T` về `soup800.fs` 24-sector)* |

### 1.2. Nhóm 15 Stock đã chạy thực tế trên thân máy Canon 6D 1.1.6
Xác nhận từ nhật ký `ML/LOGS/FILMSIM.LOG` và `ML/LOGS/FILMSIM.PREV.LOG` trên thẻ SD (`/Volumes/EOS_DIGITAL`), các stock sau đã được chọn qua Film Shelf, bake bảng 3D lattice `(y,u,v)` (`17×16×16`), ghi Picture Style vào `User Def 1` và chụp/develop trên máy:

1. **CineStill 50D** (`ML/FILM/50D.fs`)
2. **CineStill 800T** (`ML/FILM/soup800.fs`)
3. **Fujifilm Pro 400H** (`ML/FILM/400H.fs`)
4. **Fujifilm Superia 400** (`ML/FILM/SUP400.fs`)
5. **Fujifilm C200** (`ML/FILM/C200.fs`)
6. **Kodak Portra 160** (`ML/FILM/P160.fs`)
7. **Kodak Portra 400** (`ML/FILM/portra.fs`)
8. **Kodak Portra 800** (`ML/FILM/P800.fs`)
9. **Kodak Ektar 100** (`ML/FILM/EK100.fs`)
10. **Kodak Gold 200** (`ML/FILM/gold.fs`)
11. **Kodak ColorPlus 200** (`ML/FILM/CP200.fs`)
12. **Kodak UltraMax 400** (`ML/FILM/UMAX.fs`)
13. **Ilford HP5 Plus** (`ML/FILM/HP5.fs`)
14. **Fujifilm Astia 100F** (`ML/FILM/astia.fs`)
15. **Fujifilm Classic Neg** (`ML/FILM/classicneg.fs`) & **Classic Chrome** (`ML/FILM/classicchrome.fs`)

### 1.3. Nhóm 19 Stock đã có bộ Ground-Truth Dehancer (`xpan_comparisons/stocks/`)
Các stock đã có sẵn tập ảnh đối chiếu chuẩn trong repo để chạy tiếp pipeline 48-sector:
- `Acros-100`, `Cinestill-50D`, `Cinestill-800T`, `Color-Plus-200`, `Ektachrome-E100`, `Ektar-100`, `Gold-200`, `HP5-400`, `Kodachrome-64`, `Portra-160`, `Portra-160NC`, `Portra-160VC`, `Portra-400`, `Portra-800`, `Pro-400H`, `Provia-100F`, `Velvia-100`, `Velvia-50`, `XP2-400`.

---

## 2. Phương pháp Test (Test Methodology)

### 2.1. Kiểm thử & Tối ưu hóa trên Host C Engine (`grade_jpeg` / `ship3`)
- **Thực thi trực tiếp bằng C Engine của module:** Không dùng mô phỏng Python xấp xỉ; ảnh đầu vào được chạy trực tiếp qua mã C thực tế của `film_sim` (`se_look_apply` cho đường RGB Develop và `fs_core_process_rows3` với 3D YUV lattice `17×16×16` cho đường chụp trực tiếp / LiveView).
- **Hàm mục tiêu khoảng cách màu (ISO/CIE 11664-6 CIEDE2000 — $\Delta E_{00}$):**
  - Tách biệt sai số sắc độ ($\Delta H'$), độ bão hòa ($\Delta C'$) và độ sáng ($\Delta L'$).
  - Có hệ số xoay elip xanh dương/cyan ($R_T$) ở vùng $h \in [250^\circ, 280^\circ]$ — cực kỳ quan trọng cho màu nước biển và bầu trời của film daylight như CineStill 50D.
- **Ràng buộc vật lý bổ sung (Penalty Terms):**
  1. **Neutral Gray Balance:** Ép 6 ô xám ở hàng 4 ColorChecker giữ cân bằng $\|R - B\| \le 1.0$, không bị ám màu ở vùng trung tính.
  2. **Skin Warmth & Highlight Roll-off:** Ngăn hiện tượng cháy trắng phấn (chalky white blowout) trên vùng trán/sống mũi và giữ độ ấm hồng hào của da ($R > G > B$, tránh đảo kênh xanh lá).
  3. **Highlight Daylight Tint:** Giữ ánh xanh cyan-blue nhẹ ở vùng sáng cao ($B > R$) đúng chất âm bản điện ảnh Kodak Vision3 50D.
- **Kiểm tra Bit-Exactness:** Đối chiếu byte-for-byte trên toàn bộ khung hình `5472×3648` (`59,885,585 / 59,885,585` byte khớp 100%) khi tối ưu hóa tập lệnh ARM cho `fs_core_process_rows3`.

### 2.2. Kiểm thử trên thân máy Canon 6D 1.1.6 (On-Camera Verification)
- **Đường chụp trực tiếp (In-Shot YUV Hook tại `0xFF3442DC`):**
  - Kiểm tra cả 2 lần gọi hook của Canon trên cùng một lần bấm máy (Call 1: tạo ảnh preview nhúng trong `.CR2`; Call 2: tạo file `.JPG` độc lập tại cùng địa chỉ DRAM `0x4c394000`).
  - Kiểm tra trên cả tỉ lệ **Original 3:2** và các tỉ lệ có viền đen ngang/dọc (**65:24 XPan**, **2.39:1**, **16:9**, **1:1**, **4:3**, **4:5**) để đảm bảo không bị chia đôi số hàng (`1824` thay vì `3648`) hay grade đè lần 2 lên nửa trên khung hình.
- **Đường hậu kỳ RAW trên máy (`Develop` từ `.CR2`):**
  - Giải mã JPEG preview nhúng trong `.CR2`, áp dụng toàn bộ pipeline `se_look_apply` (Tone Curve + 48-sector Hue/Sat/Luma + 3-way Split Toning + Halation + Grain) và ghi ra file `_F.JPG`.

---

## 3. Các Vùng Phân đoạn dùng để Test & Chấm điểm (Test Segments)

Trong pipeline đánh giá 102 ảnh và tinh chỉnh đa miền, mỗi khung hình được tách thành **8 vùng ngữ nghĩa & vật lý** riêng biệt để đo $\Delta E_{00}$, $\Delta L^*$, $\Delta a^*$, $\Delta b^*$:

| # | Vùng kiểm thử (Segment) | Cách trích xuất / Mặt nạ (Mask) | Kênh Sector (48-sector) | Ảnh chuẩn tiêu biểu & Tiêu chí đánh giá |
| :-: | :--- | :--- | :---: | :--- |
| **1** | **Skin Tones (Da mặt & cơ thể)** | **MediaPipe Face Mesh (468 điểm mốc)** tạo đa giác khuôn mặt, loại trừ mắt, lông mày, môi và tóc; kết hợp lọc sắc thái da cho vùng cổ/tay | Sectors `1..5`<br>($7.5^\circ - 45^\circ$) | • Ảnh: `001_IMG_6883.jpg` (cosplay cầm ô), `015_IMG_8215.jpg`, `017_IMG_0573.jpg`<br>• Tiêu chí: Da vùng highlight không bị bệt trắng phấn, giữ sắc đào ấm ($R - G \in [8, 28]$), không bị ám xanh lá ($\Delta a^* > 0$).<br>• Kết quả 102 ảnh (87 ảnh có da): **Mean $\Delta E_{00} = 5.43$** ($\Delta L^* = -0.5$) |
| **2** | **Lips (Môi)** | Đa giác môi trong & ngoài từ MediaPipe Face Mesh | Sectors `0..2`, `46..47` | • Kiểm tra độ sâu sắc đỏ hồng trên môi, không bị nhợt nhạt khi hạ sáng vùng mặt.<br>• Kết quả 102 ảnh (16 ảnh cận mặt): **Mean $\Delta E_{00} = 4.76$** |
| **3** | **Hair (Tóc)** | Vùng bao quanh trán/đỉnh đầu phía trên đa giác mặt với ngưỡng độ tối và kết cấu | Đa kênh (chủ yếu Shadow/Midtone) | • Kiểm tra chi tiết vùng tối của tóc (đen/nâu/tím than ở `001_IMG_6883.jpg`), không bị bệt đen hoặc ám xanh.<br>• Kết quả 102 ảnh (15 ảnh): **Mean $\Delta E_{00} = 3.69$** |
| **4** | **Foliage Green (Cây cỏ, lá xanh)** | Mặt nạ sắc thái diệp lục ($G > R + 15$, $h \in [85^\circ, 160^\circ]$, độ bão hòa trung bình–cao) | Sectors `14..20`<br>($105^\circ - 150^\circ$) | • Ảnh: `008_IMG_7680.jpg`, `025_IMG_0130.jpg`, `057_IMG_6496.jpg`<br>• Tiêu chí: Xanh lá tự nhiên kiểu phim nhựa (organic cine green), không bị rực neon điện tử hay úa vàng.<br>• Kết quả 102 ảnh (57 ảnh): **Mean $\Delta E_{00} = 5.48$** ($\Delta L^*=-0.6, \Delta a^*=+0.6, \Delta b^*=+0.6$) |
| **5** | **Sea / Marine Water (Biển & mặt nước)** | Mặt nạ vùng xanh ngọc / xanh biển ($B > G \gg R$, $h \in [170^\circ, 245^\circ]$) | Sectors `23..27` (Cyan Sea) & `28..33` (Sky/Blue) | • Ảnh: `018_IMG_1553.jpg` (bờ biển & sóng), `095_CR3...jpg`, `100_CR3...jpg`<br>• Tiêu chí: Giữ đúng chất xanh cyan/teal đặc trưng của CineStill 50D ở vùng nước biển và đường chân trời, không bị xỉn rêu.<br>• Kết quả 102 ảnh (95 ảnh): **Mean $\Delta E_{00} = 5.93$** |
| **6** | **Shadows (Vùng tối, $Y < 50$)** | Phân ngưỡng độ chói $Y < 50$ trên toàn khung hình | `SPLIT_SH` + đường cong chân tối (`x = 0, 32, 64`) | • Tiêu chí: Độ sâu đen (black density) chắc chắn, không bị nâng sáng đục (muddy lift) hay lệch màu vùng tối.<br>• Kết quả 102 ảnh (102 ảnh): **Mean $\Delta E_{00} = 3.35$** (tốt nhất trong các phân đoạn) |
| **7** | **Midtones (Trung gian, $70 \le Y \le 170$)** | Phân ngưỡng độ chói $70 \le Y \le 170$ + Hàng 4 ColorChecker (Patches 19–24) | `SPLIT_MID` + đường cong giữa (`x = 96, 128, 160`) | • Ảnh: `015_IMG_8215.jpg` (nền gạch/đường phố phía dưới), `066_IMG_6559.jpg`<br>• Tiêu chí: Độ tương phản trung gian chuẩn xác ($\Delta L^* = \pm 0.0$), nền bê tông/nhựa đường không bị lệch đỏ hay xanh.<br>• Kết quả 102 ảnh (101 ảnh): **Mean $\Delta E_{00} = 5.36$** |
| **8** | **Highlights (Vùng sáng cao, $Y > 195$)** | Phân ngưỡng độ chói $Y > 195$ (áo trắng, mây, vùng da nắng chiếu trực tiếp) | `SPLIT_HI` + đường cong vai sáng (`x = 192, 224, 255`) | • Ảnh: `001_IMG_6883.jpg` (áo trắng, tán ô trong suốt, má/mũi dưới nắng), `053_IMG_5968.jpg`<br>• Tiêu chí: Đường cong vai (shoulder knee) nén mượt từ $x = 192..255$, giữ chi tiết vùng sáng và hơi ngả xanh dương dịu ($B > R$).<br>• Kết quả 102 ảnh (95 ảnh): **Mean $\Delta E_{00} = 6.06$** ($\Delta L^* = -3.0$) |
