# Menu redesign — bản nháp theo kiểu "máy phim"

Tham khảo cách M-Film (HNGSeries) tổ chức: không phải một menu cấu hình, mà là
**một chiếc máy phim** — nạp cuộn, chụp, tráng. Người dùng không học 60 entry;
họ học 3 hành động.

Tài liệu này là **bản nháp để bàn**, chưa có dòng code nào.

## 1. Vấn đề của menu hiện tại

```
12 entry cấp ngoài, 60 entry tổng
Film Sim · Film Shelf · Film Settings · Header · Aspect · LiveView
Saved Looks · Custom Look · Recipe Files · Fuji Recipe · Photo · Develop
```

Bốn chỗ cùng nói về "look" (`Film Shelf`, `Saved Looks`, `Custom Look`, `Fuji
Recipe`) và một chỗ nói về "file recipe" (`Recipe Files`). Người mới không thể
đoán. Mọi thứ đều ngang hàng nhau, không có khái niệm "bây giờ tôi đang ở bước
nào" — trong khi công việc thật sự có ba bước rõ ràng: **nạp phim → chụp →
tráng**.

## 2. Mô hình đề xuất: 3 màn hình

```
MENU
 └─ Phim
     ├─ 1. Cuộn phim        (đang nạp stock nào, còn bao nhiêu frame)
     ├─ 2. Chụp             (HUD trên LiveView; KHÔNG có menu khi đang chụp)
     └─ 3. Tráng            (develop + contact sheet)
 └─ Cài đặt              (chỉ những gì hiếm khi đổi)
```

Màn 1 và 3 là menu thật. Màn 2 không phải menu — nó là **HUD vẽ trên LiveView**,
bật/tắt bằng một nút, và trong lúc chụp không có entry nào để lạc.

## 3. HUD khi chụp (thay cho menu)

```
┌──────────────────────────────────────────┐
│  PORTRA 400        ISO 400    f/2.8  1/125│
│  ▓▓▓▓▓▓▓▓░░░░░░░░  12/36                 │   ← số frame còn lại
│                    ─────▲─────            │   ← kim đo sáng
└──────────────────────────────────────────┘
```

Chỉ bốn thứ: tên stock, ba thông số phơi sáng, đếm frame, kim đo sáng. Không có
chữ nào khác. Đây là thứ M-Film làm và nó đúng: khi đang chụp, menu là kẻ thù.

## 4. Chỗ đặt mọi thứ hiện có

| hiện tại | đi đâu |
|---|---|
| `Film Sim` (công tắc tổng) | **Cuộn phim** → dòng đầu: "Đang nạp: Portra 400" / "Không nạp" |
| `Film Shelf` | **Cuộn phim** → "Nạp cuộn khác" |
| `Film Settings → Stock/Level/Strength` | **Cuộn phim** → cùng một màn, ba dòng dưới |
| `Aspect` | **Cuộn phim** → "Khung" (vì nó thuộc về cuộn, không đổi giữa cuộn) |
| `LiveView` (Preview Grade, Filter, Canister Art…) | **Cài đặt → Màn hình ngắm** |
| `Header` (Film Frame, Art) | **Cài đặt → Viền ảnh** |
| `Saved Looks` (Apply Look 1..10, Share) | **Cuộn phim → Look đã lưu** (sub) |
| `Custom Look` (Slot, Open Creator) | **Cài đặt → Tạo look** |
| `Fuji Recipe` (field + WB Grid + Save) | **Cài đặt → Recipe Fuji** |
| `Recipe Files` (Import/Export/Reload) | **Cài đặt → Tệp recipe** |
| `Photo`, `Develop` (Darkroom, Auto, x4, View, Replace JPEG…) | **Tráng** |
| `Film Debug` | **Cài đặt → Chẩn đoán** (giữ nguyên nội dung) |

Nguyên tắc: **cái gì đổi mỗi lần chụp thì ở màn 1; cái gì đổi một lần rồi thôi
thì ở Cài đặt; cái gì là hành động sau khi chụp thì ở màn 3.**

## 5. Đếm frame — thứ mình chưa có

`Cuộn phim` cần biết cuộn dài bao nhiêu frame và còn bao nhiêu. Hai lựa chọn:

- **đơn giản**: đếm số ảnh đã chụp kể từ lúc nạp cuộn (config + số file trên thẻ);
- **như M-Film**: có "rewind" — hết cuộn thì bắt nạp cuộn mới, không cho chụp tiếp.

Bản nháp đề nghị **đơn giản trước**: hiện `12/36`, hết thì nhắc "cuộn hết", vẫn
cho chụp tiếp. Rewind là việc của bản sau.

## 6. Chi phí

| việc | ước lượng |
|---|---|
| Đổi tên/gộp menu (không đụng logic) | ~1 giờ, rủi ro thấp |
| HUD trên LiveView (vẽ bằng `bmp_printf`) | ~2–3 giờ, rủi ro trung bình (đè lên Canon draw) |
| Đếm frame + "cuộn hết" | ~1 giờ |
| Bỏ hẳn menu khi đang chụp | không nên làm — giữ đường thoát về menu |

## 7. Rủi ro

- HUD vẽ lên LiveView có thể **xung đột với Global Draw / Canon overlays**; mình
  đã có `Clear Canon Overlays` và biết vấn đề này.
- Gộp menu làm **đường đi quen thuộc của bạn thay đổi** — nếu bạn đang dùng
  `Film Settings → Aspect` hằng ngày thì nó sẽ ở chỗ khác.
- Không nên bắt chước M-Film ở chỗ **giấu hết**: họ bán "trải nghiệm máy phim",
  mình bán "kiểm soát màu có số đo". Giấu `Develop` đi là phản tác dụng.

## 8. Đề nghị bước đầu

Làm **mục 4 (gộp menu) trước**, không làm HUD. Đó là thay đổi rẻ, đảo ngược được,
và trả lời được câu "menu có dễ dùng hơn không" mà không cần code vẽ HUD.

Nói "làm mục 4" là tôi sửa menu thật; nói "làm cả HUD" thì tôi làm tiếp mục 5–6.
