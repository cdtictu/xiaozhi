# Xiaozhi bản nhỏ (ESP32 thường) — nối dây & kiểm tra

Bản này **độc lập hoàn toàn** với bản lớn (ESP32-S3-CAM). Mọi thứ của nó nằm trong
`~/xiaozhi/xiaozhi_nho/`. Chỉ có bộ biên dịch `esp-idf` là dùng chung, để không tốn thêm 700 MB.

| | Bản lớn | Bản nhỏ (tài liệu này) |
|---|---|---|
| Chip | ESP32-S3 (có PSRAM) | ESP32-D0WD-V3, 4 MB flash, **không PSRAM** |
| Cổng USB | chip CH343 | chip CP2102 → `/dev/ttyUSB0` |
| Gọi bằng giọng nói | được ("你好小智", "Hey Willow") | **không** — phải bấm nút |
| Bluetooth | không có (S3 không có BT Classic) | **có** BT Classic, để phát nhạc sang xe |
| Telegram / nhạc / động cơ | có | chưa có (mã nguồn gốc, sạch) |

## 1. Phần cứng cần có

- 1 board ESP32 DevKit (loại đang cắm: ESP32-WROOM-32, 38 chân)
- 1 mic I2S **INMP441**
- 1 mạch khuếch đại I2S **MAX98357A** + 1 loa nhỏ 4 Ω hoặc 8 Ω
- 1 màn hình **ST7735S 128x160** (loại 8 chân, có chân BLK)
- Dây cắm breadboard

Firmware đã được đặt sẵn đúng loại màn hình này (`CONFIG_LCD_ST7735_128X160`).
Nếu bạn dùng màn hình khác, chạy `./xz.sh menuconfig` → *Xiaozhi Assistant* → *LCD Type*.

## 2. Bảng nối dây

Số chân dưới đây lấy từ chính mã nguồn của board
(`main/boards/bread-compact-esp32-lcd/config.h`), không phải phỏng đoán.

### Mic INMP441 (thu tiếng)

| Chân mic | Nối vào ESP32 |
|---|---|
| VDD | 3V3 |
| GND | GND |
| L/R | GND (chọn kênh trái) |
| WS | GPIO **25** |
| SCK | GPIO **26** |
| SD | GPIO **32** |

### Loa MAX98357A (phát tiếng)

| Chân mạch loa | Nối vào ESP32 |
|---|---|
| VIN | 5V (chân VIN/5V của devkit) |
| GND | GND |
| DIN | GPIO **33** |
| BCLK | GPIO **14** |
| LRC | GPIO **27** |
| GAIN | để trống (9 dB) |
| +, − | hai dây loa |

### Màn hình ST7735S 128x160

| Chân màn hình | Nối vào ESP32 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SCL / SCK | GPIO **15** |
| SDA / MOSI | GPIO **4** |
| RES / RST | GPIO **18** |
| DC / A0 | GPIO **21** |
| CS | GPIO **22** |
| BLK / LED | GPIO **23** |

### Nút và đèn

| Việc | Chân |
|---|---|
| Nút BOOT — **bấm để nói** | GPIO **0** (đã có sẵn trên devkit, không cần nối) |
| Nút cảm ứng (không bắt buộc) | GPIO **5** |
| Nút hỏi nhanh (không bắt buộc) | GPIO **19** |
| Đèn LED trạng thái | GPIO **2** |

### Lưu ý nguồn

- Mic và màn hình dùng **3V3**, đừng cắm 5V — mic INMP441 sẽ chết.
- Mạch loa nên lấy **5V** để tiếng to và không kéo sụt 3V3 của ESP32.
- Cấp nguồn cho ESP32 bằng cáp USB dữ liệu tốt. Loa lúc phát to có thể kéo tụt điện làm
  ESP32 khởi động lại — nếu gặp, cắm nguồn 5V riêng cho mạch loa và nối chung GND.
- **Không nối cùng lúc hai board vào cùng bộ dây.** Bản nhỏ dùng chân khác bản lớn hoàn toàn.

## 3. Nạp và theo dõi

Dùng script riêng của bản nhỏ, nó tự chọn cổng CP2102 và **không bao giờ nạp vào board lớn**:

```bash
cd ~/xiaozhi/xiaozhi_nho
./xz.sh build      # biên dịch
./xz.sh flash      # nạp
./xz.sh monitor    # xem log (Ctrl+] để thoát)
./xz.sh menuconfig # đổi cấu hình
```

Firmware đã được nạp sẵn vào board ngày 28/09/2026 và đã chạy ổn định (0 lần khởi động lại
trong 20 giây theo dõi). Máy đang ở chế độ cấu hình Wi-Fi, điểm phát sóng tên **Xiaozhi-30E9**.

Nếu máy không thấy board: `ls /dev/serial/by-id/`. Không có gì hiện ra nghĩa là cáp chỉ có
điện chứ không truyền dữ liệu, hoặc chưa cắm. Muốn chỉ định tay:
`PORT=/dev/ttyUSB0 ./xz.sh flash`.

## 4. Kiểm tra từng chức năng theo thứ tự

Làm đúng thứ tự này, hỏng ở bước nào thì sửa bước đó rồi mới đi tiếp.

**Bước 1 — chip còn sống.** Cắm USB rồi chạy `./xz.sh monitor`. Thấy dòng chữ khởi động
ESP-ROM là chip và cáp đều tốt.

**Bước 2 — màn hình.** Nạp firmware, màn hình phải sáng và hiện mặt biểu cảm.
- Tối đen hoàn toàn: sai chân BLK (23) hoặc thiếu 3V3.
- Sáng trắng nhưng không hình: sai CS (22), DC (21) hoặc RST (18).
- Hình bị lệch/đổi màu: chọn sai loại màn hình trong `menuconfig`.

**Bước 3 — loa.** Lúc khởi động máy sẽ phát tiếng báo.
- Không tiếng nhưng log không lỗi: kiểm tra DIN (33), BCLK (14), LRC (27) và dây loa.
- Tiếng rít hoặc nhiễu: thiếu GND chung, hoặc mạch loa đang ăn 3V3 thay vì 5V.

**Bước 4 — Wi-Fi.** Lần đầu máy tự mở điểm phát Wi-Fi tên `Xiaozhi-xxxx`. Nối điện thoại
vào đó, mở `192.168.4.1`, chọn Wi-Fi nhà và nhập mật khẩu.

**Bước 5 — mic và hội thoại.** **Bấm giữ nút BOOT** rồi nói. Nhả nút, máy trả lời.
- Máy không nghe: kiểm tra WS (25), SCK (26), SD (32) và chân L/R phải xuống GND.
- Log báo nghe được nhưng trả lời sai hẳn: mic bị lệch kênh — nối lại L/R xuống GND.

Bản nhỏ **không có từ khóa đánh thức**. Đây là giới hạn cứng của chip: mô hình đánh thức
đòi phải có PSRAM, chip này không có. Muốn gọi bằng giọng nói thì phải đổi sang
module ESP32-WROVER (có 8 MB PSRAM) hoặc dùng ESP32-S3 như bản lớn.

**Bước 6 — Bluetooth.** Xem phần 5.

## 5. Bluetooth sang màn hình xe

Cách hoạt động: ESP32 làm **nguồn phát Bluetooth (A2DP source)**, đúng vai như cái điện thoại.
Xe hoặc loa Bluetooth là bên nhận. Mặc định **nhạc đi sang Bluetooth, còn giọng trợ lý vẫn ở
loa nhỏ** trên robot — để bạn vẫn nghe nó trả lời khi nhạc đang chạy trên dàn của xe.

### Điều khiển bằng giọng nói

Firmware đăng ký 5 công cụ, bạn chỉ cần nói tự nhiên, máy chủ sẽ tự gọi:

| Nói đại ý | Công cụ được gọi |
|---|---|
| "tìm loa bluetooth" | `self.bluetooth.scan` — quét 8 giây, chỉ lấy thiết bị âm thanh |
| "kết nối loa JBL" | `self.bluetooth.connect` (tên chỉ cần một phần, không cần đúng hoa thường) |
| "ngắt bluetooth" | `self.bluetooth.disconnect` |
| "bluetooth đang thế nào" | `self.bluetooth.get_state` |
| "phát mọi thứ qua bluetooth" | `self.bluetooth.set_route` với `all` / `music` / `speaker` |

### Điều khiển bằng lệnh qua cổng USB (không cần kích hoạt, không cần mic)

Đường giọng nói chỉ chạy khi máy đã kích hoạt xong ở xiaozhi.me và mic đã nối. Nên firmware
còn nhận lệnh trực tiếp qua cổng USB:

```bash
cd ~/xiaozhi/xiaozhi_nho
./xz.sh monitor        # mo log, go lenh ngay trong day
```

Gõ `bt` rồi Enter để xem hướng dẫn. Các lệnh:

| Lệnh | Việc |
|---|---|
| `bt on` | Bật Bluetooth cho lần khởi động sau, máy tự khởi động lại sau 3 giây |
| `bt off` | Tắt Bluetooth, trả RAM lại cho phần còn lại, máy khởi động lại |
| `bt scan` | Quét 8 giây, in ra tên và địa chỉ các loa tìm được |
| `bt connect JBL` | Kết nối, tên chỉ cần một phần |
| `bt disconnect` | Ngắt |
| `bt status` | Xem trạng thái và đường ra âm thanh hiện tại |
| `bt route all` | Cho cả giọng trợ lý sang Bluetooth (`music` / `speaker` để đổi lại) |

Thoát monitor bằng `Ctrl+]`.

Thứ tự làm lần đầu: `bt on` → chờ máy khởi động lại → bật loa ở chế độ ghép nối →
`bt scan` → `bt connect <tên loa>`.

### Bluetooth bật theo hai bước, và vì sao phải như vậy

Chip này có 320 KB RAM, không có PSRAM. Vùng nhớ của bộ điều khiển Bluetooth bị giữ ngay từ
lúc khởi động **dù có dùng hay không**, và nó ăn đúng 34 KB: đo được heap trống 94,9 KB khi trả
lại vùng nhớ đó, so với 60,5 KB khi giữ. Giữ mà không cần thì firmware hết RAM và chết thật —
tôi đã gặp: máy khởi động lại liên tục, `operator new` thất bại lúc tạo bộ đọc file tiếng báo.

Nên firmware quyết định lúc khởi động:

- **Chưa từng dùng Bluetooth** → trả lại toàn bộ vùng nhớ Bluetooth, máy chạy y như bản gốc.
- **Lần đầu bạn yêu cầu Bluetooth** → máy bật cờ trong NVS, nói cho bạn biết, rồi tự khởi động
  lại sau 3 giây. Yêu cầu lại lần nữa là dùng được.
- **Các lần sau** → tự bật Bluetooth và nối lại thiết bị đã ghi nhớ sau 5 giây (chờ Wi-Fi ổn định).
- **Nếu máy chết hai lần liền khi Bluetooth đang bật** → firmware tự tắt Bluetooth và ghi rõ
  lý do vào log, để không khởi động lại vô tận. Sống quá 30 giây thì nó quên các lần chết cũ.
  Muốn tắt bằng tay: `bt off`.
- **Khi chưa lưu Wi-Fi nào** → Bluetooth tự nhường chỗ ở lần khởi động đó, vì lúc mở điểm phát
  sóng cấu hình (AP + web server + DNS) là lúc firmware ngốn RAM nhất. Nên **hãy cấu hình Wi-Fi
  trước, rồi mới dùng Bluetooth.**

Thiết bị đã kết nối được ghi nhớ trong NVS nên chỉ phải ghép nối một lần.

Máy hiện tên **Xiaozhi** khi ghép nối, và tự đồng ý ghép nối (mã PIN cố định `0000` cho các
dàn xe cũ còn dùng kiểu ghép nối cũ).

### Thử theo thứ tự

1. Bật loa Bluetooth hoặc dàn xe, đưa vào **chế độ ghép nối**.
2. Nói "tìm loa bluetooth". Nghe máy đọc danh sách tên tìm được. Không thấy gì thì loa chưa ở
   chế độ ghép nối, hoặc nó đang bị điện thoại của bạn chiếm.
3. Nói "kết nối" kèm tên. Xem log qua `./xz.sh monitor`, phải thấy `Connected to ...` rồi
   `Streaming started`.
4. Cho phát nhạc. Nhạc phải ra loa xe, còn câu trả lời của trợ lý ra loa nhỏ.
5. Muốn cả giọng trợ lý sang xe: nói "phát mọi thứ qua bluetooth".

Xem log để đọc lỗi, các dòng có nhãn `BtA2dp`:
- `Not enough memory for the 24576 byte audio buffer` → hết RAM, xem phần giới hạn bên dưới.
- `Dropped a frame, the sink is not draining the buffer` → sóng yếu hoặc loa xử lý không kịp;
  thử để robot gần dàn hơn.
- `Pairing failed` → xóa Xiaozhi khỏi danh sách thiết bị bên dàn xe rồi ghép lại.

### Cách nó chạy bên trong

Chỗ nối là đúng một điểm trong đường ra âm thanh
(`main/audio/audio_service.cc`, ngay nơi ghi PCM ra loa). Khi Bluetooth đang phát và khung
tiếng là nhạc, PCM được đổi tần số 24 kHz → 44,1 kHz, nhân thành 2 kênh, rồi đẩy vào một
vòng đệm 24 KB. Bluedroid tự mã hóa SBC. Vòng đệm đầy thì hàm ghi **chặn lại** — đó chính là
cách giữ đúng nhịp phát sau khi loa I2S không còn nằm trong đường đi nữa.

### Kết quả thử thật ngày 28/09/2026: Bluetooth KHÔNG vừa RAM con chip này

Đã thử trên chính board của bạn, không phải phỏng đoán:

| Cấu hình | Heap trống lúc khởi động | Kết quả |
|---|---|---|
| Tắt Bluetooth ngay khi biên dịch (`CONFIG_BT_ENABLED=n`) | **139,9 KB** (thấp nhất 108 KB) | chạy ổn định |
| Biên dịch có Bluetooth, trả lại vùng nhớ lúc chạy | 94,3 KB | chạy được nhưng biên rất mỏng |
| Bluetooth bật thật | 56,7 KB | chết ngay khi engine âm thanh và MQTT vào việc |

Con số đáng chú ý nhất: chỉ **để Bluetooth trong bản build** đã mất 45 KB dù không dùng, vì
`esp_bt_controller_mem_release()` chỉ trả lại được một phần — phần bộ nhớ tĩnh của tầng
bluedroid và của cơ chế chạy song song Wi-Fi/Bluetooth vẫn bị giữ. Tổng cộng Bluetooth ăn
khoảng **83 KB** trong 320 KB của chip. Quá nhiều.

Chỗ chết luôn là một lần cấp phát bộ nhớ thất bại (`operator new`), rồi vì firmware tắt
exception nên nó `abort()` và khởi động lại. Cơ chế tự cứu đã bắt đúng và tự tắt Bluetooth
sau 2 lần chết — lặp lại được ở hai lượt thử độc lập.

Đã thử cắt các chỗ sau mà vẫn không đủ: bộ đệm TLS 16 KB → 8 KB, giải phóng chứng thư sau
bắt tay, tắt AMPDU Wi-Fi, bớt đệm thu/phát Wi-Fi, vùng đệm nhạc 24 KB → 12 KB, bỏ đăng ký
công cụ MCP khi Bluetooth tắt, ngăn xếp console 4 KB → 3 KB.

**Firmware đang nạp trong board là bản đã tắt Bluetooth** (`CONFIG_BT_ENABLED=n`), để máy chạy
chắc. Cấu hình có Bluetooth được giữ ở `xiaozhi-esp32/sdkconfig.bluetooth-bat`, và toàn bộ mã
Bluetooth vẫn nằm trong `main/bluetooth/` — biên dịch được, chỉ bị loại khỏi bản build khi tắt.
Muốn bật lại: `cp sdkconfig.bluetooth-bat sdkconfig && ./xz.sh build`.

### Ba đường đi tiếp, theo thứ tự tôi khuyên

1. **Mua module phát Bluetooth rời KCX_BT_EMITTER** (khoảng 60–90k). Cắm vào đường âm thanh,
   không ăn một byte RAM nào của ESP32, và không phụ thuộc firmware. Đây là cách chắc chắn
   nhất cho một sản phẩm thương mại.
2. **Đổi sang module ESP32-WROVER** (có 8 MB PSRAM, cùng chân cắm, giá chênh không nhiều).
   Vừa đủ RAM cho Bluetooth, **và** mở lại được từ khóa đánh thức mà bản nhỏ hiện không có.
   Đây là lựa chọn tốt nhất nếu bạn muốn giữ mọi thứ trong một con chip.
3. **Cắt bớt tính năng firmware** để lấy 30–40 KB: bỏ bộ ảnh cảm xúc (`FLASH_NONE_ASSETS`),
   dùng phông nhỏ hơn, giảm vùng đệm LVGL. Làm được nhưng đánh đổi phần giao diện, và biên an
   toàn vẫn mỏng — tôi không khuyên cho sản phẩm bán ra.

### Giới hạn thật, cần biết trước

- Chip không có PSRAM. Firmware chiếm 2,57 MB trong ô 3,06 MB (còn 16%). Vẫn vừa, nhưng
  không còn nhiều chỗ để thêm tính năng nặng.
- RAM là thứ chật nhất, không phải flash. Lúc chạy bình thường: heap trống 71,6 KB, thấp nhất
  50,4 KB. Bật Bluetooth thì trừ đi 34 KB nữa, nên nếu sau này thêm tính năng mà máy khởi động
  lại liên tục thì gần như chắc chắn là hết RAM — đọc log sẽ thấy `abort()` ngay sau một lần
  cấp phát bộ nhớ.
- Tôi đã giảm vùng đệm Wi-Fi (tắt AMPDU, bớt số đệm thu/phát) để chừa RAM cho Bluetooth. Tốc độ
  Wi-Fi giảm nhưng luồng nhạc Opus chỉ 32 kbps nên không ảnh hưởng. Muốn trả lại thì mở
  `./xz.sh menuconfig` → *Component config* → *Wi-Fi*.
- Wi-Fi và Bluetooth dùng chung một sóng 2,4 GHz nên phải chia nhau thời gian phát.
- **Bản nhỏ chưa có nguồn nhạc.** Trình phát nhạc (máy chủ nhạc trong nhà, tải từ YouTube) là
  phần tôi viết thêm cho bản lớn, bản nhỏ này đang là mã nguồn gốc. Muốn thử Bluetooth ngay
  thì chuyển sang chế độ `all` để chính giọng trợ lý đi qua Bluetooth — nghe được tiếng ở loa
  xe là đường Bluetooth đã thông.
- Nếu về sau RAM quá chật, phương án chắc chắn hơn là **module phát Bluetooth rời
  (KCX_BT_EMITTER)** cắm vào đường âm thanh, không phụ thuộc RAM của ESP32.
