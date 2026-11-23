# S9 HI2 — một lệnh Windows, tự chạy toàn bộ lượt thử, không cần PuTTY

Đây là hướng dẫn chính cho bản kernel `s9/home-idle-oneboot-ab`, chế độ
`hi=2`. Không phải gõ lệnh `usb>` thủ công hoặc tự đọc log để canh chuyển
pha. Script trực tiếp thay thế PuTTY trên cổng USB COM.

## Chuẩn bị và an toàn

1. Build **BOOT + SYSTEM cùng phiên bản**: s-kernel
   `s9/home-idle-oneboot-ab`; installer sios
   `fix/s9-homeidle-bootarg-guard`. Full installer có thể format USERDATA;
   sao lưu BOOT, SYSTEM, EFS, USERDATA ra máy tính trước.
2. Boot args chính xác (163 byte nhập vào, 201 byte sau khi workflow thêm
   `shub=6 bt=7 fps_log=1 metal=2 camtl=0`, dưới ngân sách 280 byte):
   ```text
   hi=2 mali_gov=1 mali_gov_frame=450 mali_probe=1 p3power=1 poster_probe=1 max77705_fg_s=10 max77705_diag=1 max77705_diag_s=30 glstage=1 glcost=1 p0hash=1 p0verify=1
   ```
3. Trên Windows, để `s9-home-idle-serial-auto.ps1` và
   `s9-home-idle-report.ps1` **cùng thư mục** (hoặc chạy trong checkout
   của nhánh kernel đúng commit). Xem Windows Device Manager để biết COM
   của cáp USB serial; các lần thử trước dùng COM125.
4. **ĐÓNG PuTTY** trước khi chạy: Windows không cho hai chương trình
   đồng thời sở hữu cổng COM. Giữ màn hình sáng, mở khóa (không chạm
   khi bắt đầu tự đo), để brightness/Wi-Fi/nguồn sạc ở cùng điều kiện.

## Chỉ một lệnh

Mở PowerShell tại thư mục gốc s-kernel và chạy:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\s9-home-idle-serial-auto.ps1 -Port COM125 -Execute
```

Không cần đóng/mở lại PuTTY giữa pha, không cần gửi thêm `sysctl`.
`-Execute` là quyền đồng ý chạy thật; **không có bước nhập xác nhận
bằng bàn phím** sau đó. Nếu COM là COM7, đổi `-Port COM125` thành
`-Port COM7`. Script không ghi đè tên log đã tồn tại.

Để xem trước mà không kết nối hoặc chạy giả lập kiểm thử:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\s9-home-idle-serial-auto.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\s9-home-idle-serial-auto.ps1 -ReplayLog .\tools\tests\s9-home-idle-auto-fixture.log
```

## Những bước script tự làm

- Mở COM độc quyền, ghi **mọi byte log thô**, kể cả khi màn hình không vẽ
  FPS. Kiểm tra đúng echo của USB và giá trị sysctl, sau đó gửi
  `hi_cmd=201` để chặn trạng thái cũ, `hi_cmd=250` kiểm tra phản hồi
  `hi: ping mode=2` từ SpringBoard. **ACK kernel đơn lẻ không đủ.**
  Nếu USB `EPERM`, `ENOENT`, thiếu thư viện hoặc sai kernel thì dừng
  ngay, giữ nguyên log, không giả tạo nhãn pha.
- Gửi `button home` để đưa giao diện về Home. Gửi `hi_cmd=252` để
  re-arm **một** lượt A/B/A/B, kể cả đã lỡ lượt tự động sau boot.
- Đợi có `hi: auto=START` và đủ chuỗi
  `baseline → cut1 → recover1 → cut2 → recover2 → FINISHED`,
  **20 giây/pha**, do chính SpringBoard đo thời gian. Theo dõi
  `cut > 0`, FPS, CPU `backboardd`, `hi: ctx`, `hi: clock_view`,
  `hi: scene`. Nếu pha tự hủy thì bỏ các cửa sổ cũ khỏi kết quả và chỉ
  chấp nhận một chuỗi hoàn chỉnh; nếu hết thời hạn thì thông báo thất bại.
- Gửi đúng lệnh hỗ trợ `swipe l 1 350 1000`, đợi
  `usb: swipe: done`; sau đó `hi_cmd=2`, bắt buộc có
  `hi: mark=secondary`, đo Home phụ 65 giây.
- Gửi `hi_cmd=300` để SpringBoard thử mở Settings bằng
  `SBSOpenSensitiveURLAndUnlock(prefs:root=General)` trên luồng nền;
  nếu thất bại thì thử helper `battery_test open prefs:root=General`.
  Nếu 32 giây chưa có `hi: scene ... settings_confirmed=1`, script
  **tự thử đường helper lần nữa bằng `hi_cmd=301`**. Chỉ sau xác nhận
  Settings *thực sự ở foreground* mới gửi `hi_cmd=3` lấy
  `hi: mark=settings`, rồi đo 65 giây. Thất bại cả hai đường thì lưu
  phép thử đã có nhưng báo `incomplete`, **không coi đó là dữ liệu
  Settings hợp lệ**.
- Trong mọi trường hợp kết thúc, gửi `hi_cmd=201` dừng thử và đóng COM
  (callback còn có hard deadline 20 giây trong mã). Không thay đổi GPU
  voltage, governor, screen power, bootarg hoặc dữ liệu người dùng.
  Nếu sensor pin báo **47°C trở lên**, dừng an toàn.

## Kết quả xuất ra tự động

Khi chạy xong (kể cả kết thúc sớm), sẽ có tối đa ba file cùng tiền tố:

- `putty-S9-hi2-auto-YYYYMMDD-HHMMSS.log`: **toàn bộ raw serial** không
  trộn dòng của máy tính. Đây là file quan trọng nhất để phân tích.
- `putty-S9-hi2-auto-YYYYMMDD-HHMMSS.events.txt`: thời điểm gửi/ACK
  từng lệnh, START, CUT, RECOVER, FINISHED và lỗi nếu có.
- `putty-S9-hi2-auto-YYYYMMDD-HHMMSS.report.txt`: báo cáo tổng hợp
  FPS, CPU, xung GPU, context commits và callback per-phase, được script
  tự chạy từ `tools/s9-home-idle-report.ps1`.

Gửi **.log và .report.txt**. Không cần tự cắt log, đọc từng dòng hay
đoán khoảng thời gian A/B. Nếu màn hình Settings không mở, gửi luôn
`.events.txt` để biết đường nào bị từ chối.

## Giới hạn xác minh trước flash

Các bài CI kiểm tra cú pháp C/iOS SDK, state machine A/B và parser trên
Windows, script PowerShell preview/replay, nguồn shell installer, giới
hạn boot-args, source kernel và `CTLFLAG_KERN`. Tuy vậy vẫn cần phép
thử phần cứng để xác nhận một icon Clock bị loại khỏi Home có còn tạo
compositor commit hay không, và Settings helper có hoạt động trên
chính image người dùng flash hay không. Trên thiết bị, mọi lệnh điều
khiển phải trả về đúng ACK và SpringBoard marker; không xác nhận thì
không suy diễn thành kết quả.
