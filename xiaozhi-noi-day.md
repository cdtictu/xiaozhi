# Xiaozhi ESP32-S3 — Sơ đồ nối dây
**Board target: `bread-compact-wifi-s3cam`** (có camera), biến thể có động cơ:
**`bread-compact-wifi-s3cam-motor`**
Phần cứng: Freenove ESP32-S3-WROOM N16R8 (16MB flash / 8MB Octal PSRAM)
MAC: 80:b5:4e:c6:ac:4c

> CẢNH BÁO: cấu hình này dùng GPIO 19/20 cho màn hình, tức là
> **cổng USB native không còn hoạt động**. Từ giờ nạp và xem log
> qua cổng USB-UART (CH343). Script xz.sh đã tự chọn đúng cổng.

## 1. Mic INMP441 (I2S vào)

| INMP441 | ESP32-S3 | Ghi chú |
|---------|----------|---------|
| VDD | 3V3 | KHÔNG cắm 5V |
| GND | GND | |
| L/R | GND | chọn kênh trái — bắt buộc |
| WS  | GPIO 1  | |
| SCK | GPIO 2  | chân này có LED onboard, sẽ nhấp nháy — bình thường |
| SD  | GPIO 42 | |

## 2. Ampli MAX98357A (I2S ra)

| MAX98357A | ESP32-S3 | Ghi chú |
|-----------|----------|---------|
| VIN  | 5V | |
| GND  | GND | |
| DIN  | GPIO 39 | |
| BCLK | GPIO 40 | |
| LRC  | GPIO 41 | |
| SD   | nối lên 3V3 | chip có sẵn trở kéo XUỐNG 100kΩ bên trong. Module không có trở kéo lên 1MΩ thì để trống = TẮT loa |
| GAIN | bỏ trống | 9dB |
| +/-  | loa 4-8Ω | |

## 3. Màn hình TFT ST7735S 1.8" 128x160 (SPI)

| Chân TFT | ESP32-S3 | Ghi chú |
|----------|----------|---------|
| VCC | 3V3 | |
| GND | GND | |
| CS   | GPIO 45 | |
| RESET| GPIO 21 | **khi bật động cơ TC1508A: chuyển sang 3V3** (xem mục 11) |
| A0 / DC / RS | GPIO 47 | |
| SDA / MOSI | GPIO 20 | ĐÂY LÀ CHÂN USB D- |
| SCK / CLK  | GPIO 19 | ĐÂY LÀ CHÂN USB D+ |
| LED / BL   | GPIO 38 | |

## 4. Camera OV2640

Cắm thẳng module vào đầu FPC có sẵn trên board Freenove.
KHÔNG phải nối dây — firmware dùng đúng bộ chân mặc định của board:

D0=11  D1=9  D2=8  D3=10  D4=12  D5=18  D6=17  D7=16
XCLK=15  PCLK=13  VSYNC=6  HREF=7  SIOC=5  SIOD=4
PWDN, RESET = không dùng

## 5. Nút bấm & LED

| Chức năng | GPIO |
|-----------|------|
| BOOT (bấm để nói) | 0, có sẵn |
| LED WS2812 | 48, có sẵn |

Bản này không có nút Touch và Volume +/-.

## 6. Bảng chân — toàn bộ 45 GPIO

| GPIO | Dùng làm gì |
|------|-------------|
| 0 | nút BOOT |
| 1, 2, 42 | mic INMP441 |
| 3 | động cơ phải IN3 **hoặc** đo pin qua ADC1 — chỉ chọn một (chân strapping) |
| 4,5,6,7,8,9,10,11,12,13,15,16,17,18 | camera |
| 14 | động cơ trái IN1 |
| 19, 20 | màn hình (mất USB native) |
| 21 | động cơ trái IN2 (trước là RESET màn hình) |
| 38, 45, 47 | màn hình |
| 35, 36, 37 | PSRAM — cấm động |
| 39, 40, 41 | ampli MAX98357A |
| 43, 44 | UART0 — log ra cổng CH343 |
| 46 | động cơ phải IN4 (chân strapping) |
| 48 | LED WS2812 |

Không còn chân trống nào. Khe thẻ SD (38/39/40) không dùng được nữa.

## 7. Lưu ý nguồn

Camera + loa + Wi-Fi cùng lúc hút dòng khá cao. Dùng củ sạc ≥1A.
Nếu ESP reset lúc phát tiếng to → thêm tụ 470-1000µF vào chân 5V
của MAX98357A.

## 8. Lệnh hay dùng

```bash
~/xiaozhi/xz.sh build     # build lai
~/xiaozhi/xz.sh flash     # nap (tu chon cong UART)
~/xiaozhi/xz.sh monitor   # xem log, Ctrl+] de thoat
~/xiaozhi/xz.sh restore   # tra lai firmware TestData cu
```

## 9. Bot Telegram (@Xiaozhi0_bot)

Chỉ chat ID chủ (5835460689) mới ra lệnh được. Token và chat ID nằm trong
`sdkconfig` (menuconfig → Xiaozhi Assistant → Telegram Bot), file này không bị commit.

| Lệnh | Tác dụng |
|------|----------|
| /status | trạng thái, RAM, âm lượng, Wi-Fi |
| /thongbao <nội dung> | hiện chữ lên màn hình + tiếng báo |
| /anh [chú thích] | chụp ảnh OV2640 gửi về Telegram |
| /volume [0-100] | xem / đặt âm lượng |
| /dosang [0-100] | xem / đặt độ sáng màn hình |
| /giaodien toi\|sang | đổi giao diện tối / sáng (tối hợp với GIF Otto nền đen) |
| /bieucam <tên> | hiện thử một biểu cảm, vd `/bieucam music`; báo lỗi nếu assets không có |
| /phude on\|off | bật / tắt chữ hội thoại dưới biểu cảm (thông báo hệ thống vẫn hiện) |
| /theodoi on\|off | chuyển tiếp mọi câu hội thoại về Telegram |
| /nhac, /nhac <số\|tên>, /nhac dung | danh sách / phát / dừng nhạc từ máy tính (mục 12) |
| /nhac server <url> | đổi địa chỉ máy chủ nhạc khi IP máy tính thay đổi |
| /dichuyen tien\|lui\|trai\|phai\|dung [giây] [tốc độ] | chạy robot (bản có động cơ), vd `/dichuyen tien 1.5 80` |
| /reboot | khởi động lại |

Thêm/bớt lệnh: sửa hàm `RegisterCommands()` trong
`xiaozhi-esp32/main/telegram/telegram_bot.cc` (mỗi lệnh là một dòng `AddCommand`),
menu lệnh trên Telegram tự cập nhật khi ESP khởi động.

## 10. Emoji động trên màn hình

Đang dùng bộ GIF Otto thu nhỏ 96x96: `~/xiaozhi/emoji/otto-gif-96/`
(menuconfig → Xiaozhi Assistant → Custom Emoji Directory).
Tên file = tên biểu cảm (`neutral.gif`, `happy.gif`, `sad.gif`...), đổi GIF khác thì
thay file cùng tên rồi `xz.sh build && xz.sh flash`.
Muốn quay lại emoji tĩnh mặc định: để trống Custom Emoji Directory.
GIF Otto nền đen → `/giaodien toi` hoặc nói "đổi sang giao diện tối".
Khi đang phát nhạc, màn hình hiện `music.gif`; phát xong tự về `neutral.gif`.
Muốn đổi hình lúc phát nhạc: thay file `music.gif` trong thư mục emoji (giữ đúng tên).

## 11. Động cơ — mạch cầu H mini TC1508A

| TC1508A | Nối tới | Ghi chú |
|---------|---------|---------|
| VCC (+) | cực + pin riêng cho động cơ | KHÔNG lấy từ chân 3V3 của ESP |
| GND (−) | cực − pin động cơ **và** GND của ESP | bắt buộc chung GND |
| IN1 | GPIO 14 | động cơ trái |
| IN2 | GPIO 21 | động cơ trái |
| IN3 | GPIO 3  | động cơ phải |
| IN4 | GPIO 46 | động cơ phải |
| MOTOR-A (2 dây) | động cơ trái | |
| MOTOR-B (2 dây) | động cơ phải | |

**Trước khi nạp firmware có động cơ:** chuyển dây RESET của màn hình từ GPIO 21 sang 3V3.
Nếu quên, GPIO 21 giữ RESET ở mức thấp → màn hình trắng (không hỏng, nối lại là chạy).

Lưu ý:
- Nguồn động cơ: dùng pin riêng (4×AA hoặc 1 cell Li-ion), xem điện áp tối đa
  in trên module. Động cơ khởi động hút dòng lớn, dùng chung nguồn USB dễ làm ESP reset.
- Chống nhiễu: hàn tụ gốm 100nF giữa 2 chân mỗi động cơ, thêm tụ 100-470µF ở VCC
  của module. Nhiễu động cơ có thể lọt vào mic.
- Bánh quay ngược chiều → đảo 2 dây của động cơ đó.
- Động cơ giật lúc ESP khởi động → thêm trở 10kΩ từ mỗi chân IN xuống GND.
- Tốc độ dưới ~30% động cơ có thể không đủ lực quay.

Bật/tắt trong firmware: menuconfig → Xiaozhi Assistant →
Bread Compact Wi-Fi + LCD + Camera Options → TC1508A dual DC motor driver.

Điều khiển:
- Giọng nói: "tiến lên", "lùi lại", "quay trái", "quay phải", "dừng lại"
  (Xiaozhi gọi công cụ `self.robot.move` / `self.robot.stop`).
- Telegram: `/dichuyen tien 1.5 80` (1,5 giây, tốc độ 80%).
- Mỗi lần chạy tự dừng sau tối đa 5 giây.

## 12. Nhạc — phát từ máy tính qua Wi-Fi

1. Cài ffmpeg (một lần): `sudo apt install ffmpeg`
2. Bỏ file nhạc (mp3, m4a, flac, wav...) vào `~/xiaozhi/music`
   — KHÔNG bỏ vào thư mục `ogg/` bên trong (đó là chỗ script ghi file đã đổi;
   file lỡ bỏ vào đó sẽ được tự chuyển ra ngoài)
3. Chạy máy chủ nhạc và để cửa sổ mở (Ctrl+C để tắt):
   `python3 ~/xiaozhi/music_server.py`
   File mới bỏ vào thư mục tự có trong danh sách sau khoảng 10 giây.
4. Phát nhạc:
   - Nói: "có những bài nào", "mở bài ...", "tắt nhạc"
   - Telegram: `/nhac` (danh sách), `/nhac 2`, `/nhac <tên bài>`, `/nhac dung`
   - Đang phát: bấm BOOT hoặc gọi wake word để dừng.
5. Thêm bài từ điện thoại: gửi file nhạc (mp3, m4a... tối đa 20 MB) cho @Xiaozhi0_bot.
   Chú thích (caption) của tin nhắn sẽ là tên bài; không có thì lấy tên file.
   Máy tính tải về `~/xiaozhi/music`, đổi sang Opus rồi bot nhắn "Đã thêm … — bài số N".
   Máy tính phải đang chạy music_server.py và đã cài ffmpeg.

Bài đã đổi xong vẫn nằm trong danh sách kể cả khi file gốc bị xóa hay đổi tên.
Muốn bỏ hẳn một bài: xóa file `.ogg` tương ứng trong `~/xiaozhi/music/ogg/`.

Địa chỉ máy chủ nhạc lưu trong bộ nhớ trong của ESP. **IP máy tính đổi thì chỉ cần nhắn**
`/nhac server http://IP-moi:8080/`, không phải build lại. Xem địa chỉ đang dùng: `/nhac server`.
Địa chỉ mặc định khi chưa đặt gì nằm ở menuconfig → Xiaozhi Assistant → Music Player.
Máy tính và ESP phải cùng mạng Wi-Fi. Nhạc được đổi sang Opus 24 kHz mono 32 kbps
(khoảng 240 KB mỗi phút), bài gốc trong `~/xiaozhi/music` giữ nguyên.

## 13. Pin — hiện mức pin trên thanh trạng thái

Thanh trạng thái xếp sẵn: **Wi-Fi bên trái → giờ ở giữa → tắt tiếng + pin bên phải**.
Biểu tượng pin chỉ hiện khi board đo được pin, nên phải nối mạch chia áp:

| Nối | Ghi chú |
|-----|---------|
| Cực + pin → điện trở 100kΩ → GPIO 3 | điện trở "trên" |
| GPIO 3 → điện trở 100kΩ → GND | điện trở "dưới" |
| Cực − pin → GND của ESP | bắt buộc chung GND |

Pin 1 cell Li-ion (tối đa 4,2V) qua cầu chia 100k/100k còn khoảng 2,1V ở GPIO 3,
nằm trong tầm đo của ADC. **Không đưa quá 3,3V vào chân GPIO.**

Khi pin gần cạn: màn hình chuyển sang mặt buồn ngủ (`sleepy.gif`), hiện thông báo pin yếu
và phát tiếng báo. Pin đầy lại thì tự về mặt thường.

Bật sau khi đã nối dây: `xz.sh menuconfig` → Xiaozhi Assistant →
Bread Compact Wi-Fi + LCD + Camera Options → *Battery level on GPIO3*,
rồi `xz.sh build && xz.sh flash`. Đổi điện trở khác thì sửa 2 giá trị kΩ ngay dưới mục đó.

GPIO 3 dùng chung với chân IN3 của mạch động cơ TC1508A, nên **chỉ bật được một
trong hai**. Bật cả hai thì build sẽ báo lỗi rõ ràng.
