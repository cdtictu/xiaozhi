#include "music_library.h"

#include <cJSON.h>
#include <esp_log.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "application.h"
#include "board.h"
#include "cjson_utils.h"
#include "mcp_server.h"
#include "network_interface.h"
#include "settings.h"

#if CONFIG_TELEGRAM_BOT_ENABLE
#include "telegram/telegram_bot.h"
#endif

#define TAG "MusicLibrary"

namespace {

constexpr int kConnectId = 3;
constexpr int kHttpTimeoutMs = 5000;
constexpr int kYoutubeTimeoutMs = 180000;

// "/nhac server <url>" stores the address in NVS, so a new IP needs no rebuild
std::string BaseUrl() {
    Settings settings("music", false);
    std::string url = settings.GetString("url", CONFIG_MUSIC_SERVER_URL);
    if (url.empty()) {
        url = CONFIG_MUSIC_SERVER_URL;
    }
    if (url.back() != '/') {
        url += '/';
    }
    return url;
}

std::string Trim(const std::string& text) {
    size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

std::string ToLowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return text;
}

}  // namespace

MusicLibrary& MusicLibrary::GetInstance() {
    static MusicLibrary instance;
    return instance;
}

void MusicLibrary::Initialize() {
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddTool("self.music.list",
                       "List the songs on the user's music server. Use it to find the exact song "
                       "name before calling `self.music.play`.",
                       PropertyList(), [this](const PropertyList& properties) -> ToolResult {
                           auto songs = FetchSongs();
                           if (!songs) {
                               return std::unexpected(songs.error());
                           }
                           CJsonUniquePtr list(cJSON_CreateArray());
                           for (const auto& song : *songs) {
                               cJSON_AddItemToArray(list.get(), cJSON_CreateString(song.name.c_str()));
                           }
                           CJsonStringUniquePtr text(cJSON_PrintUnformatted(list.get()));
                           return std::string(text.get());
                       });
    mcp_server.AddTool(
        "self.music.play",
        "Play a song from the user's music server on the speaker. The song starts after you finish "
        "speaking and ends the conversation; the user stops it with the button or the wake word.\n"
        "Args:\n"
        "  `name`: the song name from `self.music.list`, part of it, or its number in the list.\n"
        "A name that is not in the library is searched for on YouTube and downloaded, which "
        "takes up to a minute.",
        PropertyList({Property("name", kPropertyTypeString)}),
        [this](const PropertyList& properties) -> ToolResult {
            auto played = Play(properties["name"].value<std::string>());
            if (!played) {
                return std::unexpected(played.error());
            }
            return std::string("Playing: ") + *played;
        });
    mcp_server.AddTool("self.music.stop", "Stop the song that is playing", PropertyList(),
                       [](const PropertyList& properties) -> ToolResult {
                           Application::GetInstance().StopAudioUrl();
                           return true;
                       });

#if CONFIG_TELEGRAM_BOT_ENABLE
    auto& bot = TelegramBot::GetInstance();
    bot.AddCommand("nhac",
                   "Nhạc: /nhac (danh sách), /nhac <số|tên>, /nhac dung, /nhac ytb <link>, "
                   "/nhac server <url>. Gửi file nhạc để thêm bài",
                   [this](const std::string& args) { return HandleTelegram(args); });
    bot.SetAudioHandler([this](const std::string& file_path, const std::string& name) {
        return AddFromTelegram(file_path, name);
    });
#endif
}

#if CONFIG_TELEGRAM_BOT_ENABLE
std::string MusicLibrary::AddFromTelegram(const std::string& file_path, const std::string& name) {
    // Only the Telegram file path travels on the LAN; the server reads the bot token itself
    CJsonUniquePtr params(cJSON_CreateObject());
    cJSON_AddStringToObject(params.get(), "file_path", file_path.c_str());
    cJSON_AddStringToObject(params.get(), "name", name.c_str());
    CJsonStringUniquePtr body(cJSON_PrintUnformatted(params.get()));

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(kConnectId);
    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("Content-Type", "application/json");
    http->SetContent(std::string(body.get()));
    if (auto opened = http->Open("POST", BaseUrl() + "add"); !opened) {
        return "❌ Không kết nối được máy chủ nhạc " + BaseUrl() +
               " (máy tính đã chạy music_server.py chưa?)";
    }
    auto status = http->GetStatusCode();
    http->Close();
    if (!status || (*status != 200 && *status != 202)) {
        return "❌ Máy chủ nhạc không nhận file (đang chạy bản music_server.py cũ?)";
    }
    return "📥 Đang tải “" + name + "” về máy tính và chuyển đổi, xong sẽ báo lại.";
}
#endif

std::string MusicLibrary::AddFromYoutube(const std::string& url) {
    CJsonUniquePtr params(cJSON_CreateObject());
    cJSON_AddStringToObject(params.get(), "url", url.c_str());
    CJsonStringUniquePtr body(cJSON_PrintUnformatted(params.get()));

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(kConnectId);
    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("Content-Type", "application/json");
    http->SetContent(std::string(body.get()));
    if (auto opened = http->Open("POST", BaseUrl() + "youtube"); !opened) {
        return "❌ Không kết nối được máy chủ nhạc " + BaseUrl();
    }
    auto status = http->GetStatusCode();
    http->Close();
    if (!status || (*status != 200 && *status != 202)) {
        return "❌ Máy chủ nhạc từ chối link (link phải là YouTube, và máy tính cần bản "
               "music_server.py mới).";
    }
    return "📥 Đang tải tiếng từ YouTube về máy tính, xong sẽ báo lại.";
}

std::expected<MusicLibrary::Song, std::string> MusicLibrary::FetchFromYoutube(
    const std::string& query) {
    CJsonUniquePtr params(cJSON_CreateObject());
    cJSON_AddStringToObject(params.get(), "query", query.c_str());
    cJSON_AddBoolToObject(params.get(), "wait", true);
    CJsonStringUniquePtr body(cJSON_PrintUnformatted(params.get()));

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(kConnectId);
    http->SetTimeout(kYoutubeTimeoutMs);
    http->SetHeader("Content-Type", "application/json");
    http->SetContent(std::string(body.get()));
    if (auto opened = http->Open("POST", BaseUrl() + "youtube"); !opened) {
        return std::unexpected("Không kết nối được máy chủ nhạc " + BaseUrl());
    }
    auto status = http->GetStatusCode();
    if (!status) {
        http->Close();
        return std::unexpected("Máy chủ nhạc không trả lời khi tải từ YouTube");
    }
    std::string response = http->ReadAll();
    http->Close();

    CJsonUniquePtr root(cJSON_Parse(response.c_str()));
    auto name = cJSON_GetObjectItem(root.get(), "name");
    auto file = cJSON_GetObjectItem(root.get(), "file");
    if (*status != 200 || !cJSON_IsString(name) || !cJSON_IsString(file)) {
        auto error = cJSON_GetObjectItem(root.get(), "error");
        return std::unexpected(cJSON_IsString(error) ? error->valuestring
                                                     : "Không tải được bài từ YouTube");
    }
    ESP_LOGI(TAG, "Fetched from YouTube: %s", file->valuestring);
    return Song{name->valuestring, file->valuestring};
}

std::expected<std::vector<MusicLibrary::Song>, std::string> MusicLibrary::FetchSongs() {
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(kConnectId);
    http->SetTimeout(kHttpTimeoutMs);
    if (auto opened = http->Open("GET", BaseUrl() + "index.json"); !opened) {
        ESP_LOGW(TAG, "Cannot reach %s: %s", BaseUrl().c_str(), opened.error().ToString().c_str());
        return std::unexpected("Không kết nối được máy chủ nhạc " + BaseUrl() +
                               " (máy tính đã chạy music_server.py chưa?)");
    }
    auto status = http->GetStatusCode();
    if (!status) {
        http->Close();
        return std::unexpected("Máy chủ nhạc " + BaseUrl() + " không trả lời (hết giờ chờ)");
    }
    if (*status != 200) {
        http->Close();
        // A wrong address often reaches some other device that answers 404/403
        return std::unexpected("Địa chỉ " + BaseUrl() + "index.json trả mã " +
                               std::to_string(*status) +
                               ", có thể đang gọi nhầm thiết bị khác.\nĐổi: /nhac server http://IP:8080/");
    }
    std::string body = http->ReadAll();
    http->Close();

    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    auto list = cJSON_GetObjectItem(root.get(), "songs");
    if (!cJSON_IsArray(list)) {
        return std::unexpected("index.json trên máy chủ nhạc không hợp lệ");
    }
    std::vector<Song> songs;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach (item, list) {
        auto name = cJSON_GetObjectItem(item, "name");
        auto file = cJSON_GetObjectItem(item, "file");
        if (cJSON_IsString(name) && cJSON_IsString(file)) {
            songs.push_back({name->valuestring, file->valuestring});
        }
    }
    return songs;
}

std::expected<MusicLibrary::Song, std::string> MusicLibrary::FindSong(const std::string& query) {
    auto songs = FetchSongs();
    if (!songs) {
        return std::unexpected(songs.error());
    }
    if (songs->empty()) {
        return std::unexpected("Thư mục nhạc đang trống");
    }

    // A number picks the song from the /nhac list
    char* end = nullptr;
    long index = strtol(query.c_str(), &end, 10);
    if (end != query.c_str() && *end == '\0') {
        if (index >= 1 && index <= static_cast<long>(songs->size())) {
            return (*songs)[index - 1];
        }
        return std::unexpected("Không có bài số " + query);
    }

    // Exact name first, then part of the name. File names are accent-free slugs,
    // so "cap bao" also finds "cấp báo...".
    std::string wanted = ToLowerAscii(query);
    std::string wanted_slug = wanted;
    std::replace(wanted_slug.begin(), wanted_slug.end(), ' ', '-');
    const Song* partial = nullptr;
    for (const auto& song : *songs) {
        std::string name = ToLowerAscii(song.name);
        if (name == wanted) {
            return song;
        }
        if (partial == nullptr && (name.find(wanted) != std::string::npos ||
                                   song.file.find(wanted_slug) != std::string::npos)) {
            partial = &song;
        }
    }
    if (partial != nullptr) {
        return *partial;
    }
    // The list helps both the user and the model pick an existing name
    std::string message = "Không tìm thấy bài: " + query + "\nDanh sách bài có thể phát:\n";
    for (size_t i = 0; i < songs->size() && i < 20; i++) {
        message += std::to_string(i + 1) + ". " + (*songs)[i].name + "\n";
    }
    return std::unexpected(message);
}

std::expected<std::string, std::string> MusicLibrary::Play(const std::string& query) {
    auto song = FindSong(query);
    if (!song) {
        // A number means "song N in the list"; only names are worth searching for
        bool is_number = !query.empty() &&
                         query.find_first_not_of("0123456789") == std::string::npos;
        if (is_number) {
            return std::unexpected(song.error());
        }
        ESP_LOGI(TAG, "%s not in the library, searching YouTube", query.c_str());
        auto fetched = FetchFromYoutube(query);
        if (!fetched) {
            return std::unexpected(fetched.error());
        }
        song = *fetched;
    }
    ESP_LOGI(TAG, "Play %s", song->file.c_str());
    Application::GetInstance().PlayAudioUrl(BaseUrl() + song->file);
    return song->name;
}

std::string MusicLibrary::HandleTelegram(const std::string& args) {
    if (args.rfind("ytb ", 0) == 0 || args.rfind("youtube ", 0) == 0) {
        std::string url = Trim(args.substr(args.find(' ') + 1));
        if (url.rfind("http", 0) != 0) {
            return "Cách dùng: /nhac ytb <link YouTube>";
        }
        return AddFromYoutube(url);
    }
    if (args == "server") {
        return "🖥 Máy chủ nhạc: " + BaseUrl() + "\nĐổi: /nhac server http://IP:8080/";
    }
    if (args.rfind("server ", 0) == 0) {
        std::string url = Trim(args.substr(7));
        if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
            return "Cách dùng: /nhac server http://192.168.1.10:8080/";
        }
        if (url.back() != '/') {
            url += '/';
        }
        Settings settings("music", true);
        settings.SetString("url", url);
        auto songs = FetchSongs();
        if (!songs) {
            return "⚠️ Đã lưu " + url + " nhưng chưa kết nối được.\n" + songs.error();
        }
        return "✅ Máy chủ nhạc: " + url + "\nThấy " + std::to_string(songs->size()) + " bài.";
    }
    if (args == "dung" || args == "dừng" || args == "stop") {
        Application::GetInstance().StopAudioUrl();
        return "⏹ Đã dừng nhạc.";
    }
    if (args.empty()) {
        auto songs = FetchSongs();
        if (!songs) {
            return "❌ " + songs.error();
        }
        if (songs->empty()) {
            return "Thư mục nhạc đang trống. Bỏ file MP3 vào ~/xiaozhi/music trên máy tính.";
        }
        std::string reply = "🎵 Danh sách bài hát:\n";
        for (size_t i = 0; i < songs->size(); i++) {
            reply += std::to_string(i + 1) + ". " + (*songs)[i].name + "\n";
        }
        return reply + "\nPhát: /nhac <số> hoặc /nhac <tên>";
    }
    auto played = Play(args);
    if (!played) {
        return "❌ " + played.error();
    }
    return "▶️ Phát: " + *played + "\nDừng: /nhac dung hoặc bấm nút BOOT";
}
