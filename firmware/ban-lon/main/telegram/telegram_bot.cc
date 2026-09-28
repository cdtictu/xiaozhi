#include "telegram_bot.h"

#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "application.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "backlight.h"
#include "board.h"
#include "camera.h"
#include "display.h"
#include "lvgl_theme.h"
#include "network_interface.h"
#include "settings.h"

#define TAG "TelegramBot"

namespace {

constexpr const char* kApiUrl = "https://api.telegram.org/bot" CONFIG_TELEGRAM_BOT_TOKEN "/";
constexpr int kConnectId = 4;
constexpr int kPollTimeoutS = 5;
constexpr int kRequestTimeoutMs = 15000;
constexpr int kUploadTimeoutMs = 30000;
constexpr size_t kUploadChunkSize = 8 * 1024;
constexpr size_t kMaxMessageBytes = 4000;  // Telegram limit is 4096 characters
constexpr size_t kMaxChatLogBytes = 3500;
constexpr int kMinBackoffS = 5;
constexpr int kMaxBackoffS = 60;
constexpr uint32_t kTaskStackSize = 8192;

std::string Trim(const std::string& text) {
    size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// Cut at a UTF-8 character boundary so the result stays valid text
void TruncateUtf8(std::string& text, size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return;
    }
    size_t end = max_bytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        end--;
    }
    text.resize(end);
}

bool ParseInt(const std::string& text, int min, int max, int& value) {
    char* end = nullptr;
    long parsed = strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || parsed < min || parsed > max) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

const char* StateText(DeviceState state) {
    switch (state) {
        case kDeviceStateStarting:
            return "Đang khởi động";
        case kDeviceStateWifiConfiguring:
            return "Đang cấu hình Wi-Fi";
        case kDeviceStateIdle:
            return "Đang chờ";
        case kDeviceStateConnecting:
            return "Đang kết nối";
        case kDeviceStateListening:
            return "Đang nghe";
        case kDeviceStateSpeaking:
            return "Đang nói";
        case kDeviceStateNotifying:
            return "Đang phát thông báo";
        case kDeviceStateUpgrading:
            return "Đang cập nhật";
        case kDeviceStateActivating:
            return "Đang kích hoạt";
        case kDeviceStateAudioTesting:
            return "Đang test âm thanh";
        case kDeviceStateFatalError:
            return "Lỗi nghiêm trọng";
        default:
            return "Không rõ";
    }
}

// Flatten the board's status JSON into "key.sub: value" lines
void AppendJson(std::string& out, const cJSON* object, const std::string& prefix) {
    for (const cJSON* child = object->child; child != nullptr; child = child->next) {
        if (child->string == nullptr) {
            continue;
        }
        std::string key = prefix.empty() ? child->string : prefix + "." + child->string;
        if (cJSON_IsObject(child)) {
            AppendJson(out, child, key);
        } else if (cJSON_IsString(child)) {
            out += key + ": " + child->valuestring + "\n";
        } else if (cJSON_IsNumber(child)) {
            char number[32];
            snprintf(number, sizeof(number), "%g", child->valuedouble);
            out += key + ": " + number + "\n";
        } else if (cJSON_IsBool(child)) {
            out += key + ": " + (cJSON_IsTrue(child) ? "true" : "false") + "\n";
        }
    }
}

}  // namespace

TelegramBot& TelegramBot::GetInstance() {
    static TelegramBot instance;
    return instance;
}

TelegramBot::TelegramBot() : owner_chat_id_(CONFIG_TELEGRAM_CHAT_ID) {}

TelegramBot::~TelegramBot() = default;

void TelegramBot::Start() {
    if (started_.exchange(true)) {
        return;
    }
    if (strlen(CONFIG_TELEGRAM_BOT_TOKEN) == 0) {
        ESP_LOGW(TAG, "Bot token is empty, Telegram bot disabled");
        return;
    }
    // Internal-RAM stack: commands write NVS, which must not run on a PSRAM stack
    BaseType_t created = xTaskCreate(
        [](void* arg) {
            static_cast<TelegramBot*>(arg)->PollTask();
            vTaskDelete(nullptr);
        },
        "telegram", kTaskStackSize, this, 1, nullptr);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Telegram task");
    }
}

void TelegramBot::OnChatMessage(const char* role, const std::string& text) {
    if (!running_ || owner_chat_id_.empty() || !forward_chat_) {
        return;
    }
    std::lock_guard<std::mutex> lock(chat_mutex_);
    // Bounded: drop lines while Telegram is unreachable
    if (chat_log_.size() > kMaxChatLogBytes) {
        return;
    }
    chat_log_ += strcmp(role, "user") == 0 ? "🧑 " : "🤖 ";
    chat_log_ += text;
    chat_log_ += "\n";
}

void TelegramBot::AddCommand(std::string name, std::string description, CommandHandler handler) {
    commands_.push_back({std::move(name), std::move(description), std::move(handler)});
}

// Built-in command table. Telegram command names: lowercase a-z, 0-9 and '_' only.
void TelegramBot::RegisterCommands() {
    auto& board = Board::GetInstance();

    AddCommand("help", "Danh sách lệnh", [this](const std::string&) { return HandleHelp(); });
    AddCommand("status", "Trạng thái thiết bị",
               [this](const std::string&) { return HandleStatus(); });
    AddCommand("thongbao", "Hiện thông báo lên màn hình: /thongbao <nội dung>",
               [this](const std::string& args) { return HandleNotify(args); });
    if (board.GetCamera() != nullptr) {
        AddCommand("anh", "Chụp ảnh bằng camera: /anh [chú thích]",
                   [this](const std::string& args) { return HandlePhoto(args); });
    }
    AddCommand("volume", "Âm lượng: /volume hoặc /volume <0-100>",
               [this](const std::string& args) { return HandleVolume(args); });
    if (board.GetBacklight() != nullptr) {
        AddCommand("dosang", "Độ sáng màn hình: /dosang hoặc /dosang <0-100>",
                   [this](const std::string& args) { return HandleBrightness(args); });
    }
#ifdef HAVE_LVGL
    if (auto display = board.GetDisplay(); display != nullptr && display->GetTheme() != nullptr) {
        AddCommand("giaodien", "Giao diện màn hình: /giaodien toi|sang",
                   [this](const std::string& args) { return HandleTheme(args); });
        AddCommand("bieucam", "Thử biểu cảm trên màn hình: /bieucam music",
                   [this](const std::string& args) { return HandleEmotion(args); });
    }
#endif
    if (auto display = board.GetDisplay(); display != nullptr) {
        AddCommand("phude", "Phụ đề dưới biểu cảm: /phude on|off",
                   [display](const std::string& args) -> std::string {
                       Settings settings("display", true);
                       bool hide;
                       if (args == "on" || args == "bat" || args == "bật") {
                           hide = false;
                       } else if (args == "off" || args == "tat" || args == "tắt") {
                           hide = true;
                       } else {
                           bool hidden = settings.GetBool("hide_subtitle", false);
                           return std::string("💬 Phụ đề đang ") + (hidden ? "TẮT" : "BẬT") +
                                  ".\nDùng /phude on hoặc /phude off";
                       }
                       display->SetHideSubtitle(hide);
                       settings.SetBool("hide_subtitle", hide);
                       return hide ? "🔕 Đã tắt phụ đề (thông báo hệ thống vẫn hiện)."
                                   : "💬 Đã bật phụ đề.";
                   });
    }
    AddCommand("theodoi", "Chuyển tiếp hội thoại về Telegram: /theodoi on|off",
               [this](const std::string& args) { return HandleForward(args); });
    AddCommand("wifi", "Đổi Wi-Fi: /wifi doi (bot sẽ mất kết nối để cấu hình lại mạng)",
               [this](const std::string& args) { return HandleWifi(args); });
    AddCommand("reboot", "Khởi động lại thiết bị",
               [this](const std::string&) { return HandleReboot(); });
}

void TelegramBot::PollTask() {
    // Commands added by the board before Start() go after the built-in ones
    auto board_commands = std::move(commands_);
    commands_.clear();
    RegisterCommands();
    commands_.insert(commands_.end(), std::make_move_iterator(board_commands.begin()),
                     std::make_move_iterator(board_commands.end()));
    {
        Settings settings("telegram", false);
        forward_chat_ = settings.GetBool("forward", false);
    }
    // Re-apply /phude off after a reboot; only when set, so assets config still works
    if (auto display = Board::GetInstance().GetDisplay(); display != nullptr) {
        Settings settings("display", false);
        if (settings.GetBool("hide_subtitle", false)) {
            display->SetHideSubtitle(true);
        }
    }
    running_ = true;

    if (owner_chat_id_.empty()) {
        ESP_LOGW(TAG, "CONFIG_TELEGRAM_CHAT_ID is empty: send /start to the bot to get it");
    }

    bool online = false;
    int backoff_s = kMinBackoffS;
    while (true) {
        bool ok;
        if (!online) {
            // Drop commands queued while offline, so a /reboot is never replayed
            ok = SkipPendingUpdates();
            if (ok) {
                online = true;
                PublishCommands();
                const auto* app_desc = esp_app_get_description();
                SendMessage(std::string("🟢 Xiaozhi đã online\nBoard: ") + BOARD_NAME +
                            "\nFirmware: " + app_desc->version + "\nGõ /help để xem lệnh.");
            }
        } else {
            FlushChatLog();
            ok = PollUpdates();
        }

        if (ok) {
            backoff_s = kMinBackoffS;
        } else {
            vTaskDelay(pdMS_TO_TICKS(backoff_s * 1000));
            backoff_s = std::min(backoff_s * 2, kMaxBackoffS);
        }
    }
}

bool TelegramBot::SkipPendingUpdates() {
    CJsonUniquePtr params(cJSON_CreateObject());
    cJSON_AddNumberToObject(params.get(), "offset", -1);
    cJSON_AddNumberToObject(params.get(), "timeout", 0);
    auto root = CallApi("getUpdates", params.get(), kRequestTimeoutMs);
    if (!root) {
        return false;
    }
    auto result = cJSON_GetObjectItem(root.get(), "result");
    const cJSON* update = nullptr;
    cJSON_ArrayForEach (update, result) {
        auto id = cJSON_GetObjectItem(update, "update_id");
        if (cJSON_IsNumber(id)) {
            next_update_id_ = static_cast<int64_t>(id->valuedouble) + 1;
        }
    }
    return true;
}

bool TelegramBot::PollUpdates() {
    CJsonUniquePtr params(cJSON_CreateObject());
    if (next_update_id_ > 0) {
        cJSON_AddNumberToObject(params.get(), "offset", static_cast<double>(next_update_id_));
    }
    cJSON_AddNumberToObject(params.get(), "timeout", kPollTimeoutS);
    cJSON_AddNumberToObject(params.get(), "limit", 10);
    auto allowed = cJSON_AddArrayToObject(params.get(), "allowed_updates");
    cJSON_AddItemToArray(allowed, cJSON_CreateString("message"));

    auto root = CallApi("getUpdates", params.get(), kPollTimeoutS * 1000 + kRequestTimeoutMs);
    if (!root) {
        return false;
    }
    auto result = cJSON_GetObjectItem(root.get(), "result");
    const cJSON* update = nullptr;
    cJSON_ArrayForEach (update, result) {
        auto id = cJSON_GetObjectItem(update, "update_id");
        if (cJSON_IsNumber(id)) {
            next_update_id_ = static_cast<int64_t>(id->valuedouble) + 1;
        }
        HandleUpdate(update);
    }
    return true;
}

void TelegramBot::HandleUpdate(const cJSON* update) {
    auto message = cJSON_GetObjectItem(update, "message");
    auto chat = cJSON_GetObjectItem(message, "chat");
    auto chat_id = cJSON_GetObjectItem(chat, "id");
    if (!cJSON_IsNumber(chat_id)) {
        return;
    }

    std::string from = std::to_string(static_cast<long long>(chat_id->valuedouble));
    if (owner_chat_id_.empty()) {
        SendMessage(from, "Chat ID của bạn là: " + from +
                              "\nĐiền số này vào CONFIG_TELEGRAM_CHAT_ID (menuconfig → Xiaozhi "
                              "Assistant → Telegram Bot) rồi build và nạp lại.");
        return;
    }
    if (from != owner_chat_id_) {
        ESP_LOGW(TAG, "Ignoring message from chat %s", from.c_str());
        return;
    }
    auto text = cJSON_GetObjectItem(message, "text");
    if (cJSON_IsString(text)) {
        RunCommand(Trim(text->valuestring));
    } else {
        HandleAudio(message);
    }
}

void TelegramBot::HandleAudio(const cJSON* message) {
    // Bot API getFile only serves files up to 20 MB
    constexpr double kMaxDownloadBytes = 20.0 * 1024 * 1024;

    // A music file, a voice note, or an audio file sent as a document
    const cJSON* file = cJSON_GetObjectItem(message, "audio");
    if (file == nullptr) {
        file = cJSON_GetObjectItem(message, "voice");
    }
    if (file == nullptr) {
        auto document = cJSON_GetObjectItem(message, "document");
        auto mime = cJSON_GetObjectItem(document, "mime_type");
        if (cJSON_IsString(mime) && strncmp(mime->valuestring, "audio/", 6) == 0) {
            file = document;
        }
    }
    if (file == nullptr) {
        return;  // Photos, stickers...: nothing to do
    }
    if (!audio_handler_) {
        SendMessage("Chưa bật chức năng nhạc (menuconfig → Music Player).");
        return;
    }
    auto file_id = cJSON_GetObjectItem(file, "file_id");
    if (!cJSON_IsString(file_id)) {
        return;
    }
    auto size = cJSON_GetObjectItem(file, "file_size");
    if (cJSON_IsNumber(size) && size->valuedouble > kMaxDownloadBytes) {
        SendMessage("❌ File quá lớn: bot Telegram chỉ tải được file tối đa 20 MB.");
        return;
    }

    // Song name: caption, then "performer - title", then the file name without extension
    std::string name;
    auto caption = cJSON_GetObjectItem(message, "caption");
    auto title = cJSON_GetObjectItem(file, "title");
    auto performer = cJSON_GetObjectItem(file, "performer");
    auto file_name = cJSON_GetObjectItem(file, "file_name");
    if (cJSON_IsString(caption)) {
        name = Trim(caption->valuestring);
    }
    if (name.empty() && cJSON_IsString(title)) {
        name = title->valuestring;
        if (cJSON_IsString(performer)) {
            name = std::string(performer->valuestring) + " - " + name;
        }
    }
    if (name.empty() && cJSON_IsString(file_name)) {
        name = file_name->valuestring;
        if (size_t dot = name.rfind('.'); dot != std::string::npos && dot > 0) {
            name.resize(dot);
        }
    }
    if (name.empty()) {
        name = "Ghi âm";
    }

    CJsonUniquePtr params(cJSON_CreateObject());
    cJSON_AddStringToObject(params.get(), "file_id", file_id->valuestring);
    auto root = CallApi("getFile", params.get(), kRequestTimeoutMs);
    auto result = root ? cJSON_GetObjectItem(root.get(), "result") : nullptr;
    auto file_path = cJSON_GetObjectItem(result, "file_path");
    if (!cJSON_IsString(file_path)) {
        SendMessage("❌ Không lấy được file từ Telegram.");
        return;
    }
    ESP_LOGI(TAG, "Audio file received: %s", file_path->valuestring);
    std::string reply = audio_handler_(file_path->valuestring, name);
    if (!reply.empty()) {
        SendMessage(std::move(reply));
    }
}

void TelegramBot::RunCommand(const std::string& text) {
    if (text.empty()) {
        return;
    }
    // The official server rejects text sent as a wake word, so plain text is not a question
    if (text[0] != '/') {
        SendMessage("Xiaozhi chỉ nhận câu hỏi bằng giọng nói. Gõ /help để xem lệnh.");
        return;
    }

    size_t split = text.find_first_of(" \n");
    std::string name = text.substr(1, split == std::string::npos ? std::string::npos : split - 1);
    std::string args = split == std::string::npos ? "" : Trim(text.substr(split + 1));
    // Group chats append the bot name: /status@Xiaozhi0_bot
    if (size_t at = name.find('@'); at != std::string::npos) {
        name.resize(at);
    }
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (name == "start") {
        name = "help";
    }

    auto it = std::find_if(commands_.begin(), commands_.end(),
                           [&name](const Command& command) { return command.name == name; });
    if (it == commands_.end()) {
        SendMessage("Không có lệnh /" + name + ". Gõ /help để xem danh sách.");
        return;
    }
    ESP_LOGI(TAG, "Command /%s", name.c_str());
    std::string reply = it->handler(args);
    if (!reply.empty()) {
        SendMessage(std::move(reply));
    }
}

void TelegramBot::FlushChatLog() {
    std::string log;
    {
        std::lock_guard<std::mutex> lock(chat_mutex_);
        log.swap(chat_log_);
    }
    if (!log.empty()) {
        SendMessage(std::move(log));
    }
}

bool TelegramBot::OpenRequest(const char* method, const std::string& content_type, int timeout_ms,
                              const char* body) {
    if (!http_) {
        http_ = Board::GetInstance().GetNetwork()->CreateHttp(kConnectId);
        // Reuse the TLS session between polls instead of a handshake every few seconds
        http_->SetKeepAlive(true);
    }
    http_->SetTimeout(timeout_ms);
    http_->SetHeader("Content-Type", content_type);
    if (body != nullptr) {
        http_->SetContent(std::string(body));
    }
    // Never log the URL: it contains the bot token
    if (auto opened = http_->Open("POST", std::string(kApiUrl) + method); !opened) {
        ESP_LOGW(TAG, "%s: connect failed: %s", method, opened.error().ToString().c_str());
        http_.reset();
        return false;
    }
    return true;
}

CJsonUniquePtr TelegramBot::ReadResponse(const char* method) {
    auto status = http_->GetStatusCode();
    if (!status) {
        ESP_LOGW(TAG, "%s: no response: %s", method, status.error().ToString().c_str());
        http_.reset();
        return nullptr;
    }
    std::string response = http_->ReadAll();
    CJsonUniquePtr root(cJSON_Parse(response.c_str()));
    if (!root) {
        ESP_LOGW(TAG, "%s: invalid response, HTTP %d", method, *status);
        http_.reset();
        return nullptr;
    }
    if (*status != 200 || !cJSON_IsTrue(cJSON_GetObjectItem(root.get(), "ok"))) {
        auto description = cJSON_GetObjectItem(root.get(), "description");
        ESP_LOGW(TAG, "%s failed: HTTP %d %s", method, *status,
                 cJSON_IsString(description) ? description->valuestring : "");
        return nullptr;
    }
    return root;
}

CJsonUniquePtr TelegramBot::CallApi(const char* method, const cJSON* params, int timeout_ms) {
    CJsonStringUniquePtr body(cJSON_PrintUnformatted(params));
    if (!body || !OpenRequest(method, "application/json", timeout_ms, body.get())) {
        return nullptr;
    }
    return ReadResponse(method);
}

bool TelegramBot::SendMessage(const std::string& chat_id, std::string text) {
    if (chat_id.empty() || text.empty()) {
        return false;
    }
    TruncateUtf8(text, kMaxMessageBytes);
    CJsonUniquePtr params(cJSON_CreateObject());
    cJSON_AddStringToObject(params.get(), "chat_id", chat_id.c_str());
    cJSON_AddStringToObject(params.get(), "text", text.c_str());
    return CallApi("sendMessage", params.get(), kRequestTimeoutMs) != nullptr;
}

bool TelegramBot::SendPhoto(const std::string& jpeg, const std::string& caption) {
    const std::string boundary = "----XiaozhiTelegramBoundary";
    if (!OpenRequest("sendPhoto", "multipart/form-data; boundary=" + boundary, kUploadTimeoutMs,
                     nullptr)) {
        return false;
    }

    std::string head;
    head += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"chat_id\"\r\n\r\n";
    head += owner_chat_id_ + "\r\n";
    head += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"caption\"\r\n\r\n";
    head += caption + "\r\n";
    head += "--" + boundary +
            "\r\nContent-Disposition: form-data; name=\"photo\"; filename=\"xiaozhi.jpg\"\r\n"
            "Content-Type: image/jpeg\r\n\r\n";
    bool ok = static_cast<bool>(http_->Write(head.data(), head.size()));
    for (size_t offset = 0; ok && offset < jpeg.size(); offset += kUploadChunkSize) {
        size_t len = std::min(kUploadChunkSize, jpeg.size() - offset);
        ok = static_cast<bool>(http_->Write(jpeg.data() + offset, len));
    }
    std::string tail = "\r\n--" + boundary + "--\r\n";
    ok = ok && http_->Write(tail.data(), tail.size()) && http_->Write("", 0);
    if (!ok) {
        ESP_LOGW(TAG, "sendPhoto: upload failed");
        http_.reset();
        return false;
    }
    return ReadResponse("sendPhoto") != nullptr;
}

bool TelegramBot::PublishCommands() {
    CJsonUniquePtr params(cJSON_CreateObject());
    auto list = cJSON_AddArrayToObject(params.get(), "commands");
    for (const auto& command : commands_) {
        auto item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "command", command.name.c_str());
        cJSON_AddStringToObject(item, "description", command.description.c_str());
        cJSON_AddItemToArray(list, item);
    }
    return CallApi("setMyCommands", params.get(), kRequestTimeoutMs) != nullptr;
}

std::string TelegramBot::HandleHelp() {
    std::string reply = "📋 Lệnh của Xiaozhi:\n";
    for (const auto& command : commands_) {
        reply += "/" + command.name + " — " + command.description + "\n";
    }
    return reply;
}

std::string TelegramBot::HandleStatus() {
    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();

    int64_t uptime_s = esp_timer_get_time() / 1000000;
    char uptime[48];
    snprintf(uptime, sizeof(uptime), "%lldd %lldh %lldm", uptime_s / 86400, uptime_s % 86400 / 3600,
             uptime_s % 3600 / 60);

    std::string reply = std::string("📟 ") + BOARD_NAME + "\n";
    reply += std::string("Trạng thái: ") + StateText(app.GetDeviceState()) + "\n";
    reply += std::string("Firmware: ") + esp_app_get_description()->version + "\n";
    reply += std::string("Thời gian chạy: ") + uptime + "\n";
    reply += "RAM trống: " + std::to_string(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024) +
             " KB (thấp nhất " +
             std::to_string(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024) +
             " KB), PSRAM " + std::to_string(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024) +
             " KB\n";
    reply += std::string("Theo dõi hội thoại: ") + (forward_chat_ ? "bật" : "tắt") + "\n";

    CJsonUniquePtr status(cJSON_Parse(board.GetDeviceStatusJson().c_str()));
    if (status) {
        AppendJson(reply, status.get(), "");
    }
    return reply;
}

std::string TelegramBot::HandleNotify(const std::string& message) {
    if (message.empty()) {
        return "Cách dùng: /thongbao <nội dung>";
    }
    auto& app = Application::GetInstance();
    app.Schedule([&app, message]() {
        app.Alert("Telegram", message.c_str(), "happy", Lang::Sounds::OGG_VIBRATION);
    });
    return "✅ Đã hiện thông báo trên màn hình.";
}

std::string TelegramBot::HandlePhoto(const std::string& caption) {
    auto camera = Board::GetInstance().GetCamera();
    if (camera == nullptr) {
        return "Thiết bị không có camera.";
    }
    if (!camera->Capture()) {
        return "❌ Chụp ảnh thất bại.";
    }
    auto jpeg = camera->GetJpeg(80);
    if (!jpeg) {
        return "❌ " + jpeg.error();
    }
    if (!SendPhoto(*jpeg, caption.empty() ? "📷 Xiaozhi" : caption)) {
        return "❌ Gửi ảnh thất bại.";
    }
    return "";
}

std::string TelegramBot::HandleVolume(const std::string& args) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (args.empty()) {
        return "🔊 Âm lượng hiện tại: " + std::to_string(codec->output_volume());
    }
    int volume = 0;
    if (!ParseInt(args, 0, 100, volume)) {
        return "Cách dùng: /volume <0-100>";
    }
    codec->SetOutputVolume(volume);
    return "🔊 Đã đặt âm lượng " + std::to_string(volume);
}

std::string TelegramBot::HandleBrightness(const std::string& args) {
    auto backlight = Board::GetInstance().GetBacklight();
    if (args.empty()) {
        return "💡 Độ sáng hiện tại: " + std::to_string(backlight->brightness());
    }
    int brightness = 0;
    if (!ParseInt(args, 0, 100, brightness)) {
        return "Cách dùng: /dosang <0-100>";
    }
    backlight->SetBrightness(static_cast<uint8_t>(brightness), true);
    return "💡 Đã đặt độ sáng " + std::to_string(brightness);
}

#ifdef HAVE_LVGL
std::string TelegramBot::HandleTheme(const std::string& args) {
    auto display = Board::GetInstance().GetDisplay();
    std::string name;
    if (args == "toi" || args == "tối" || args == "dark") {
        name = "dark";
    } else if (args == "sang" || args == "sáng" || args == "light") {
        name = "light";
    } else {
        std::string current = display->GetTheme()->name() == "dark" ? "tối" : "sáng";
        return "🎨 Giao diện đang " + current + ".\nDùng /giaodien toi hoặc /giaodien sang";
    }
    auto theme = LvglThemeManager::GetInstance().GetTheme(name);
    if (theme == nullptr) {
        return "❌ Không tìm thấy giao diện " + name;
    }
    // The main task owns the UI; SetTheme also saves the choice to NVS
    Application::GetInstance().Schedule([display, theme]() { display->SetTheme(theme); });
    return name == "dark" ? "🌙 Đã chuyển sang giao diện tối." : "☀️ Đã chuyển sang giao diện sáng.";
}

// Shows one emoji from the assets, to check a new .gif without playing anything
std::string TelegramBot::HandleEmotion(const std::string& args) {
    if (args.empty()) {
        return "Cách dùng: /bieucam music (hoặc happy, sad, thinking, neutral...)";
    }
    auto display = Board::GetInstance().GetDisplay();
    auto theme = static_cast<LvglTheme*>(display->GetTheme());
    auto collection = theme != nullptr ? theme->emoji_collection() : nullptr;
    if (collection == nullptr) {
        return "Màn hình không có bộ biểu cảm.";
    }
    if (collection->GetEmojiImage(args.c_str()) == nullptr) {
        return "❌ Assets không có biểu cảm “" + args + "”.";
    }
    Application::GetInstance().Schedule(
        [display, args]() { display->SetEmotion(args.c_str()); });
    return "🙂 Đang hiện “" + args + "” trên màn hình. Lần đổi trạng thái sau sẽ trở lại bình thường.";
}
#endif

// Wi-Fi setup without holding the boot button: the device drops off the network,
// so the confirmation word avoids losing contact by a typo
std::string TelegramBot::HandleWifi(const std::string& args) {
    if (args != "doi" && args != "đổi" && args != "ok") {
        return "⚠️ Lệnh này ngắt Wi-Fi hiện tại, bot sẽ mất kết nối cho tới khi bạn đặt mạng mới.\n"
               "Chắc chắn thì gõ: /wifi doi";
    }
    // Reply while the network is still up
    SendMessage("📶 Đang vào chế độ cấu hình Wi-Fi.\n"
                "Trên điện thoại, nối vào điểm phát tên “Xiaozhi-…” rồi chọn mạng mới.\n"
                "Đặt xong máy tự nối lại và bot sẽ online.");
    if (!Board::GetInstance().StartNetworkConfiguration()) {
        return "❌ Board này không hỗ trợ cấu hình Wi-Fi.";
    }
    return "";
}

std::string TelegramBot::HandleForward(const std::string& args) {
    if (args == "on" || args == "bat" || args == "bật") {
        forward_chat_ = true;
    } else if (args == "off" || args == "tat" || args == "tắt") {
        forward_chat_ = false;
    } else {
        return std::string("Theo dõi hội thoại đang ") + (forward_chat_ ? "BẬT" : "TẮT") +
               ".\nDùng /theodoi on hoặc /theodoi off";
    }
    Settings settings("telegram", true);
    settings.SetBool("forward", forward_chat_);
    return forward_chat_ ? "👀 Đã bật: mọi câu nói với Xiaozhi sẽ được chuyển về đây."
                         : "🙈 Đã tắt theo dõi hội thoại.";
}

std::string TelegramBot::HandleReboot() {
    SendMessage("🔄 Đang khởi động lại...");
    auto& app = Application::GetInstance();
    app.Schedule([&app]() { app.Reboot(); });
    return "";
}
