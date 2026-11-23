# P7 trên Galaxy S9 — thử nguồn G3D thật và BIG CPU_OFF, riêng từng lượt

**Không cần build/flash lại.** Bản BOOT+SYSTEM P7 đã cài trong lần đo
`putty-S9-hi2-auto-20261123-023058.log` và dữ liệu thu hồi
`putty-S9-hi2-auto-20261123-024358.log` chứa các sysctl phần cứng cần
thiết. **Chỉ cập nhật script Windows** trong nhánh kernel
`s9/p7-clock-basepower-oneboot`.

Đây là kiểm tra có điều kiện tắt nguồn thật, **không phải tối ưu ổn định
đã xác nhận**. Kết quả cũ: khi rút cáp và màn hình được yêu cầu ON, pin xả
khoảng 653 mA, G3D domain/PLL vẫn ON, 8 CPU online, G3D ~68 C. Cả hai
thử nghiệm dưới đây cần xác nhận PMU/CPU_ON; **không có phép đo watt riêng
G3D hay CPU cluster** nếu chỉ cắm USB để ghi serial.

## Chuẩn bị chung

- Đảm bảo recovery/rollback đã thử được. Nếu gặp lỗi khởi động khi
  bật nguồn phần cứng, không thể cam kết script hay watchdog cứu được.
- Cho máy nguội trước khi thử. Mặc định dừng khi BIG >=82 C,
  LITTLE >=75 C, G3D >=77 C; chỉ kiểm tra nhiệt pin khi có
  dữ liệu cảm biến pin hợp lệ. Nếu 59–61 C trước đây là **nhiệt pin
  hoặc nhiệt bề mặt**, không chạy các thử nghiệm này.
- **Bật màn hình và mở khóa** trước mỗi lượt; để máy ở trạng thái không
  đang cập nhật hay mở ứng dụng nặng. Giữ USB/COM kết nối xuyên suốt khi
  CPU_OFF hoặc G3D đang OFF; không rút cáp trong cửa sổ phần cứng.
- **Đóng PuTTY**. Trong PowerShell Windows, đứng tại thư mục checkout
  đúng nhánh s-kernel đã cập nhật; thay COM125 bằng cổng hiện tại.
- Không chạy đồng thời hai cửa sổ PowerShell cùng giữ COM, không bật
  `-P7Unified`, `-ActiveG3DGate`, `-ActiveCpuPark` cùng
  `-P7PowerTest`. Mỗi lượt là một thí nghiệm riêng biệt, không chạy
  lại Clock A/B/A/B dài hoặc tự chạy lượt CPU sau G3D.
- Bạn có thể kiểm tra cú pháp và cấu hình dự kiến **không kết nối COM** bằng
  cùng lệnh bên dưới nhưng bỏ `-Execute`.

## Lượt 1: G3D thật — A/B/A, Home phụ ít tải

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\s9-home-idle-serial-auto.ps1 -Port COM125 -P7PowerTest G3D -Execute
```

Script tự nhấn Home, vuốt trái, yêu cầu xác nhận Home foreground, Clock
không phát callback 10 Hz và GPU **không có frame/job tối thiểu 15 giây**.
Nó không biết chính xác chỉ số trang Home; nếu không chứng minh được
trạng thái yên, **dừng mà không gửi gate=3**.

| Pha | Cấu hình | Bằng chứng phải có |
|---|---|---|
| `50` A1 | G3D ON, 950 mV, yên 50 giây | Domain=1, PLL=1, GPU/TZ safe, bộ đếm lỗi=0 |
| `51` B | `g3d_lab_gate=3`, không thay điện áp | Ít nhất 2 mẫu 5 giây PMU domain=0/PLL=0, tăng off_count và >=15 giây residency OFF |
| `52` A2 | Trả `gate=0`, ON 50 giây | Đọc lại domain=1 PLL=1 GPU/TZ safe, fault/wake_fail=0, off_total và wake_count tăng |

Khi B hoạt động, kernel P7 có **watchdog độc lập 120 giây**, chủ động
khôi phục G3D ON/gate0 nếu đường driver vẫn hoạt động; watchdog không
thể phục hồi một firmware deadlock hay lỗi nguồn vật lý. Script
không tuyên bố thành công chỉ từ ACK `set (0)` hoặc xung GPU giảm.

## Lượt 2: CPU BIG thật — A/B/A, màn hình OFF

**Chỉ chạy sau khi đã xem kết quả G3D và máy nguội trở lại**:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\s9-home-idle-serial-auto.ps1 -Port COM125 -P7PowerTest CPU -Execute
```

**Script đã được sửa theo log `putty-S9-p7-cpu-20261123-035500.log`.**
Bản trước gọi `hi_cmd=300` để mở Settings, nhưng SpringBoard trả
`hi: settings_open REFUSED mode=1 auto_state=aborted need_finished=1`.
Đó là điều kiện của bộ thử Clock, **không phải lỗi CPU_OFF**.

Bản hiện tại **không gọi `hi_cmd=300/301`**. Nó dùng đường Home phụ
đã được xác minh trong lượt G3D: nhấn Home, vuốt trái, đợi Home foreground
và tối thiểu 15 giây không có frame/job hay Clock 10 Hz. Sau đó kiểm
tra màn hình ON, 4 LITTLE + 4 BIG online và G3D an toàn trước A1;
script tự khóa/mở màn hình và cần `disp=0` ở các cửa sổ OFF.

Bản BOOT đang chạy phải có `sleep_cores=1`; thiếu boot arg, kernel trả
`ENODEV (19)`, script dừng, **không giả lập thành công**.

| Pha | Thao tác tự động | Xác nhận |
|---|---|---|
| `60` A1 | Home phụ yên → màn hình OFF, CPU park=0, đo 50 giây | `disp=0`, LITTLE=4 BIG=4 online |
| `61` B | Mở màn hình, bật `p7_cpu_park=1`, khóa lại, đo 50 giây | `GenericARMPE: sleep_cores: cpu N taken off (0)` và P7 BIG online giảm trong ít nhất 2 mẫu 5 giây |
| `62` A2 | Mở màn hình, tắt park, xác minh **từng lõi** `started again (0)`, khóa lại 50 giây | BIG=4 LITTLE=4 online trở lại; mở màn hình sau cùng |

Đây là `CPU_OFF`/park thật của các lõi BIG được chọn, **không phải**
CPD/SICD, CPU_SUSPEND/C2 hay system sleep. **Không có watchdog CPU_OFF
độc lập trong firmware hiện tại**; nếu điện thoại/COM treo hoàn toàn,
khôi phục từ PowerShell có thể không thành công và cần recovery.
Không rút USB trong lượt này. Sau một lần bật màn hình, hệ thống có
thể hiển thị màn hình khóa; phép thử dùng trạng thái `disp`/CPU_ON thực
và không cần Settings foreground. `disp=0` chỉ xác nhận yêu cầu màn
hình OFF từ iOS, chưa chứng minh DECON/panel đã deep-power-down.

## Tập tin kết quả

Hai lượt tự sinh tên khác nhau, không ghi đè log trước:

- `putty-S9-p7-g3d-YYYYMMDD-HHMMSS.log` hoặc
  `putty-S9-p7-cpu-YYYYMMDD-HHMMSS.log`: toàn bộ bytes serial gốc.
- `*.events.txt`: thời điểm host gửi lệnh, ACK/readback, xác minh và lý do dừng.
- `*.p7-report.txt`: tổng hợp các pha 50/51/52 hoặc 60/61/62,
  G3D off/PLL, nhiệt, CPU online, USB/touch polling và lỗi hồi phục.

Lượt này không cần `-DumpP7History`, không cần ngắt USB để kiểm tra
off/online: bằng chứng PMU và per-core được ghi trực tiếp. Dòng pin
MAX77705 lúc nối USB không đo đủ công suất SoC; dùng nhiệt theo pha và
residency để kiểm tra cơ chế, không suy số watt từ dòng pin gần 0 mA khi
cắm cáp.

## Test script không dùng thiết bị

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\s9-home-idle-serial-auto.ps1 -P7PowerTest G3D -ReplayPowerLog .\tools\tests\s9-p7-power-ab-fixture.log

powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\s9-home-idle-serial-auto.ps1 -P7PowerTest CPU -ReplayPowerLog .\tools\tests\s9-p7-power-ab-fixture.log
```

Dùng fixture `s9-p7-power-ab-negative-fixture.log` thay thế sẽ cố ý
thất bại do không có G3D OFF hoặc BIG CPU_OFF thật.
