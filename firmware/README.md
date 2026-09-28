# Mã firmware của hai bản Xiaozhi

Thư mục `xiaozhi-esp32/` (bản lớn) và `xiaozhi_nho/xiaozhi-esp32/` (bản nhỏ) là **bản sao của
kho firmware gốc** (github.com/78/xiaozhi-esp32), mỗi bản nặng vài trăm MB nên không nằm trong
kho này. Phần **tự viết thêm** thì được giữ ở đây, để nếu mất máy vẫn dựng lại được.

## Cấu trúc

| Thư mục | Nội dung |
|---|---|
| `ban-lon/` | ESP32-S3-CAM: bot Telegram, trình phát nhạc, động cơ TC1508A, pin, phụ đề |
| `ban-nho/` | ESP32 thường: Bluetooth A2DP (phát nhạc sang loa/xe) |

Trong mỗi thư mục:
- `main/...` — các file **mới hoàn toàn** do mình viết, chép nguyên si
- `changes.patch` — các **sửa đổi** trên file có sẵn của firmware gốc
- `sdkconfig` — cấu hình build đầy đủ, để dựng lại y hệt

## Dựng lại từ đầu

```bash
git clone https://github.com/78/xiaozhi-esp32
cd xiaozhi-esp32
cp -r ../firmware/ban-lon/main/* main/          # chép file moi
git apply ../firmware/ban-lon/changes.patch     # ap dung sua doi
cp ../firmware/ban-lon/sdkconfig .              # cau hinh
idf.py build
```

## Phải tự điền lại

Hai giá trị sau **đã bị xóa khỏi kho** vì là thông tin riêng, chép về rồi phải tự điền:

```
CONFIG_TELEGRAM_BOT_TOKEN="DAT_TOKEN_CUA_BAN_O_DAY"
CONFIG_TELEGRAM_CHAT_ID="DAT_CHAT_ID_CUA_BAN_O_DAY"
```

Token lấy từ @BotFather, chat ID lấy từ @userinfobot. Đừng commit hai giá trị này: ai đọc được
token là điều khiển được bot của bạn.

## Ghi chú về bản nhỏ

`ban-nho/sdkconfig` là bản **đã tắt Bluetooth** — đây là cấu hình đang chạy ổn định trên board.
`ban-nho/sdkconfig.bluetooth-bat` là bản bật Bluetooth: build được, nhưng chip ESP32 không PSRAM
**không đủ RAM để chạy**, đã thử và ghi lại số đo trong `xiaozhi_nho/noi-day-nho.md`.
