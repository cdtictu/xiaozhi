#include "bt_a2dp_source.h"

#if CONFIG_BT_A2DP_ENABLE

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstring>

#include <esp_bt.h>
#include <esp_console.h>
#include <esp_bt_main.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/task.h>

#include <ssid_manager.h>

#include "mcp_server.h"
#include "settings.h"

#define TAG "BtA2dp"

namespace {

// The A2DP stack encodes to SBC for us, and asks for stereo PCM at this rate.
constexpr int kSinkSampleRate = 44100;
constexpr int kSinkChannels = 2;
// About 70 ms of stereo audio. Bigger rides out radio hiccups better, but on this chip
// there is no spare RAM: 24 KB could not be allocated at all once Wi-Fi and TLS had taken
// their share. A full buffer stalls the decoder, which is how playback keeps its pace.
constexpr size_t kRingBytes = 12 * 1024;
// A frame is 60 ms, so waiting longer than this means the sink really is gone.
constexpr int kWriteTimeoutMs = 400;
constexpr int kDefaultScanSeconds = 8;

std::string AddressText(const esp_bd_addr_t address) {
    char text[18];
    snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", address[0], address[1],
             address[2], address[3], address[4], address[5]);
    return text;
}

bool NameMatches(const std::string& name, const std::string& wanted) {
    if (wanted.empty()) {
        return false;
    }
    auto lower = [](std::string text) {
        for (auto& c : text) {
            c = std::tolower(static_cast<unsigned char>(c));
        }
        return text;
    };
    return lower(name).find(lower(wanted)) != std::string::npos;
}

// Reads the device name out of an inquiry result, which carries it either as a
// plain property or inside the extended inquiry response.
std::string DeviceNameOf(esp_bt_gap_cb_param_t* param) {
    for (int i = 0; i < param->disc_res.num_prop; i++) {
        auto& property = param->disc_res.prop[i];
        if (property.type == ESP_BT_GAP_DEV_PROP_BDNAME) {
            return std::string(reinterpret_cast<char*>(property.val), property.len);
        }
    }
    for (int i = 0; i < param->disc_res.num_prop; i++) {
        auto& property = param->disc_res.prop[i];
        if (property.type != ESP_BT_GAP_DEV_PROP_EIR) {
            continue;
        }
        uint8_t length = 0;
        auto* eir = static_cast<uint8_t*>(property.val);
        auto* name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &length);
        if (name == nullptr) {
            name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &length);
        }
        if (name != nullptr && length > 0) {
            return std::string(reinterpret_cast<char*>(name), length);
        }
    }
    return {};
}

bool IsAudioDevice(esp_bt_gap_cb_param_t* param) {
    for (int i = 0; i < param->disc_res.num_prop; i++) {
        auto& property = param->disc_res.prop[i];
        if (property.type != ESP_BT_GAP_DEV_PROP_COD) {
            continue;
        }
        uint32_t cod = *static_cast<uint32_t*>(property.val);
        return esp_bt_gap_is_valid_cod(cod) &&
               esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_AV;
    }
    return false;
}

}  // namespace

BtA2dpSource& BtA2dpSource::GetInstance() {
    static BtA2dpSource instance;
    return instance;
}

void BtA2dpSource::GapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
    GetInstance().OnGapEvent(event, param);
}

void BtA2dpSource::A2dpCallback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t* param) {
    GetInstance().OnA2dpEvent(event, param);
}

int32_t BtA2dpSource::DataCallback(uint8_t* buffer, int32_t size) {
    return GetInstance().FillSinkBuffer(buffer, size);
}

void BtA2dpSource::Initialize() {
    // The Bluetooth controller reserves its memory at startup whether or not it is used,
    // and this chip has no PSRAM to spare. Keep it only for an owner who actually uses
    // Bluetooth; everyone else gets the memory back and a firmware that behaves like stock.
    bool wanted;
    {
        Settings settings("bt", true);
        wanted = settings.GetInt("enabled", 0) != 0 || !settings.GetString("sink_addr").empty();
        // Bluetooth holds RAM that the rest of the firmware may turn out to need. Rather
        // than reboot forever, count the crashes and give up after the second one.
        int panics = settings.GetInt("panics", 0);
        if (wanted && esp_reset_reason() == ESP_RST_PANIC) {
            panics++;
            settings.SetInt("panics", panics);
            ESP_LOGW(TAG, "Crashed %d time(s) with Bluetooth on", panics);
            if (panics >= 2) {
                ESP_LOGE(TAG, "Bluetooth does not fit in RAM here, turning it off");
                settings.SetInt("enabled", 0);
                settings.SetInt("panics", 0);
                wanted = false;
            }
        }
    }
    if (wanted) {
        // Surviving a while means this boot was fine, so forget the earlier crashes.
        xTaskCreate(
            [](void*) {
                vTaskDelay(pdMS_TO_TICKS(30000));
                Settings settings("bt", true);
                settings.SetInt("panics", 0);
                vTaskDelete(nullptr);
            },
            "bt_settle", 3072, nullptr, 1, nullptr);
    }
    // Without saved Wi-Fi the board is about to raise its setup access point, which
    // together with the web server and the DNS server is the heaviest moment of the
    // whole firmware. Bluetooth waits for the boot after Wi-Fi is set up.
    if (wanted && SsidManager::GetInstance().GetSsidList().empty()) {
        ESP_LOGW(TAG, "No Wi-Fi saved yet, standing down until the next boot");
        wanted = false;
    }
    if (wanted) {
        // BLE is not built into this firmware, so its part is free either way.
        esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    } else {
        esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
        released_.store(true);
    }
    ESP_LOGI(TAG, "Bluetooth %s, free heap %u, largest block %u",
             released_.load() ? "off (memory returned)" : "armed",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    if (released_.load()) {
        // Nothing to drive: the radio cannot come up before a reboot, and every tool
        // registered here grows the tool list this board can barely afford.
        StartConsole();
        return;
    }

    auto& mcp_server = McpServer::GetInstance();

    mcp_server.AddTool("self.bluetooth.scan",
                       "Search for nearby Bluetooth speakers and car audio systems. Returns the "
                       "names found, which can then be passed to self.bluetooth.connect.",
                       PropertyList(), [this](const PropertyList&) -> ReturnValue {
                           return Scan(kDefaultScanSeconds);
                       });

    mcp_server.AddTool(
        "self.bluetooth.connect",
        "Connect to a Bluetooth speaker or car audio system so music plays through it. "
        "`name` may be a part of the device name; leave it empty to reconnect the last one.",
        PropertyList({Property("name", kPropertyTypeString, std::string(""))}),
        [this](const PropertyList& properties) -> ReturnValue {
            return Connect(properties["name"].value<std::string>());
        });

    mcp_server.AddTool("self.bluetooth.disconnect", "Stop sending audio over Bluetooth.",
                       PropertyList(),
                       [this](const PropertyList&) -> ReturnValue { return Disconnect(); });

    mcp_server.AddTool("self.bluetooth.get_state",
                       "Report whether Bluetooth audio is connected and where audio is going.",
                       PropertyList(),
                       [this](const PropertyList&) -> ReturnValue { return StatusText(); });

    mcp_server.AddTool(
        "self.bluetooth.set_route",
        "Choose where the audio goes: `music` sends music to Bluetooth and keeps the assistant's "
        "voice on the local speaker, `all` sends both, `speaker` uses the local speaker only.",
        PropertyList({Property("mode", kPropertyTypeString, std::string("music"))}),
        [this](const PropertyList& properties) -> ReturnValue {
            return SetRoute(properties["mode"].value<std::string>());
        });

    StartConsole();

    // A remembered sink means the owner uses Bluetooth, so bring the radio up and
    // reconnect in the background instead of making them ask every boot.
    esp_bd_addr_t address;
    std::string name;
    if (LoadRememberedSink(address, name)) {
        ESP_LOGI(TAG, "Remembered sink %s, reconnecting in the background", name.c_str());
        xTaskCreate(
            [](void* argument) {
                auto* self = static_cast<BtA2dpSource*>(argument);
                vTaskDelay(pdMS_TO_TICKS(5000));  // let Wi-Fi settle first
                self->Connect("");
                vTaskDelete(nullptr);
            },
            "bt_reconnect", 4096, this, 1, nullptr);
    }
}

std::string BtA2dpSource::PeerName() const {
    std::lock_guard<std::mutex> lock(peer_mutex_);
    return peer_name_;
}

std::string BtA2dpSource::DisableForNextBoot() {
    {
        Settings settings("bt", true);
        settings.SetInt("enabled", 0);
        settings.SetInt("panics", 0);
    }
    ESP_LOGI(TAG, "Bluetooth turned off, restarting");
    xTaskCreate(
        [](void*) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_restart();
        },
        "bt_restart", 2048, nullptr, 1, nullptr);
    return "Đã tắt Bluetooth, máy khởi động lại sau 3 giây và trả RAM về cho phần còn lại.";
}

std::string BtA2dpSource::EnableForNextBoot() {
    {
        Settings settings("bt", true);
        settings.SetInt("enabled", 1);
    }
    ESP_LOGI(TAG, "Bluetooth armed for the next boot, restarting");
    xTaskCreate(
        [](void*) {
            vTaskDelay(pdMS_TO_TICKS(3000));  // let the answer reach the owner first
            esp_restart();
        },
        "bt_restart", 2048, nullptr, 1, nullptr);
    return "Bluetooth chưa được bật ở lần khởi động này để tiết kiệm RAM. Đã bật sẵn, máy sẽ "
           "khởi động lại sau 3 giây, xong rồi bạn nói lại yêu cầu này.";
}

void BtA2dpSource::StartConsole() {
    const esp_console_cmd_t command = {
        .command = "bt",
        .help = "Bluetooth: bt on | bt off | bt scan | bt connect <ten loa> | bt disconnect | "
                "bt status | bt route <speaker|music|all>",
        .hint = nullptr,
        .func = [](int argc, char** argv) -> int {
            auto& self = BtA2dpSource::GetInstance();
            std::string action = argc > 1 ? argv[1] : "status";
            std::string reply;
            if (action == "on") {
                reply = self.EnableForNextBoot();
            } else if (action == "off") {
                reply = self.DisableForNextBoot();
            } else if (action == "scan") {
                reply = self.Scan(kDefaultScanSeconds);
            } else if (action == "connect") {
                std::string name;
                for (int i = 2; i < argc; i++) {
                    if (i > 2) {
                        name += " ";
                    }
                    name += argv[i];
                }
                reply = self.Connect(name);
            } else if (action == "disconnect") {
                reply = self.Disconnect();
            } else if (action == "status") {
                reply = self.StatusText();
            } else if (action == "route") {
                reply = argc > 2 ? self.SetRoute(argv[2]) : std::string("route <speaker|music|all>");
            } else {
                reply = "bt on | bt off | bt scan | bt connect <ten loa> | bt disconnect | "
                        "bt status | bt route <speaker|music|all>";
            }
            printf("%s\n", reply.c_str());
            return 0;
        },
        .argtable = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&command));

    esp_console_repl_t* repl = nullptr;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "xiaozhi>";
    repl_config.max_cmdline_length = 128;
    repl_config.task_stack_size = 3072;
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "Type `bt` on the serial console to drive Bluetooth");
}

bool BtA2dpSource::Enable() {
    if (enabled_.load()) {
        return true;
    }
    if (released_.load()) {
        return false;
    }

    ring_ = xRingbufferCreate(kRingBytes, RINGBUF_TYPE_BYTEBUF);
    if (ring_ == nullptr) {
        ESP_LOGE(TAG, "Not enough memory for the %u byte audio buffer", (unsigned)kRingBytes);
        return false;
    }

    esp_bt_controller_config_t controller_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    auto ret = esp_bt_controller_init(&controller_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_init failed: %s", esp_err_to_name(ret));
        return false;
    }
    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_enable failed: %s", esp_err_to_name(ret));
        return false;
    }
    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_bluedroid_init failed: %s", esp_err_to_name(ret));
        return false;
    }
    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_bluedroid_enable failed: %s", esp_err_to_name(ret));
        return false;
    }

    esp_bt_gap_set_device_name("Xiaozhi");
    esp_bt_gap_register_callback(GapCallback);

    // Older car radios and speakers still use a fixed PIN.
    esp_bt_pin_code_t pin_code = {'0', '0', '0', '0'};
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, 4, pin_code);

    esp_a2d_register_callback(A2dpCallback);
    esp_a2d_source_register_data_callback(DataCallback);
    ret = esp_a2d_source_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_a2d_source_init failed: %s", esp_err_to_name(ret));
        return false;
    }

    // We always dial out, so there is no reason to be visible to others.
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

    enabled_.store(true);
    ESP_LOGI(TAG, "Bluetooth audio ready, free heap %u", (unsigned)esp_get_free_heap_size());
    return true;
}

bool BtA2dpSource::StartDiscovery(int seconds) {
    {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        devices_.clear();
    }
    // The inquiry length is counted in 1.28 second units.
    uint8_t length = static_cast<uint8_t>(std::max(1, seconds * 100 / 128));
    auto ret = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, length, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_gap_start_discovery failed: %s", esp_err_to_name(ret));
        return false;
    }
    scanning_.store(true);
    return true;
}

std::string BtA2dpSource::Scan(int seconds) {
    if (released_.load()) {
        return EnableForNextBoot();
    }
    if (!Enable()) {
        return "Không bật được Bluetooth, thiếu bộ nhớ.";
    }
    if (!StartDiscovery(seconds)) {
        return "Không bắt đầu quét được.";
    }
    for (int waited = 0; waited < seconds * 10 + 20 && scanning_.load(); waited++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    std::lock_guard<std::mutex> lock(devices_mutex_);
    if (devices_.empty()) {
        return "Không thấy thiết bị âm thanh Bluetooth nào. Hãy để loa hoặc xe ở chế độ ghép nối.";
    }
    std::string reply = "Tìm thấy:";
    for (auto& device : devices_) {
        reply += "\n- " + device.name + " (" + AddressText(device.address) + ")";
    }
    return reply;
}

bool BtA2dpSource::ConnectTo(const esp_bd_addr_t address, const std::string& name) {
    esp_bd_addr_t target;
    memcpy(target, address, sizeof(esp_bd_addr_t));
    {
        std::lock_guard<std::mutex> lock(peer_mutex_);
        memcpy(peer_address_, address, sizeof(esp_bd_addr_t));
        peer_name_ = name;
    }

    auto ret = esp_a2d_source_connect(target);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_a2d_source_connect failed: %s", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(TAG, "Connecting to %s (%s)", name.c_str(), AddressText(address).c_str());
    return true;
}

std::string BtA2dpSource::Connect(const std::string& name) {
    if (released_.load()) {
        return EnableForNextBoot();
    }
    if (!Enable()) {
        return "Không bật được Bluetooth, thiếu bộ nhớ.";
    }
    if (connected_.load()) {
        return "Đang kết nối với " + PeerName() + " rồi.";
    }

    std::string wanted = name;
    if (wanted.empty()) {
        esp_bd_addr_t address;
        std::string remembered;
        if (LoadRememberedSink(address, remembered)) {
            if (ConnectTo(address, remembered)) {
                for (int waited = 0; waited < 100 && !connected_.load(); waited++) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                return connected_.load() ? "Đã kết nối với " + peer_name_ + "."
                                         : "Không kết nối được với " + remembered +
                                               ". Hãy bật loa lên rồi thử lại.";
            }
            return "Không gọi được kết nối tới " + remembered + ".";
        }
        return "Chưa có thiết bị nào được ghi nhớ. Hãy quét rồi nói tên loa.";
    }

    // Try what the last scan already found before spending time on a new one.
    esp_bd_addr_t address;
    std::string device_name;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        for (auto& device : devices_) {
            if (NameMatches(device.name, wanted)) {
                memcpy(address, device.address, sizeof(esp_bd_addr_t));
                device_name = device.name;
                found = true;
                break;
            }
        }
    }
    if (found) {
        if (!ConnectTo(address, device_name)) {
            return "Không gọi được kết nối tới " + device_name + ".";
        }
        for (int waited = 0; waited < 100 && !connected_.load(); waited++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        return connected_.load() ? "Đã kết nối với " + PeerName() + "."
                                 : "Không kết nối được với " + device_name + ".";
    }

    // Not seen yet: scan and let the discovery handler connect the moment it shows up.
    wanted_name_ = wanted;
    if (!StartDiscovery(kDefaultScanSeconds)) {
        wanted_name_.clear();
        return "Không bắt đầu quét được.";
    }
    for (int waited = 0; waited < 200 && !connected_.load(); waited++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    wanted_name_.clear();
    if (connected_.load()) {
        return "Đã kết nối với " + PeerName() + ".";
    }
    return "Không tìm thấy thiết bị tên giống \"" + wanted +
           "\". Hãy để loa ở chế độ ghép nối rồi thử lại.";
}

std::string BtA2dpSource::Disconnect() {
    if (!connected_.load()) {
        return "Bluetooth chưa kết nối.";
    }
    esp_bd_addr_t target;
    {
        std::lock_guard<std::mutex> lock(peer_mutex_);
        memcpy(target, peer_address_, sizeof(esp_bd_addr_t));
    }
    esp_a2d_source_disconnect(target);
    return "Đã ngắt Bluetooth, tiếng quay về loa nhỏ.";
}

std::string BtA2dpSource::StatusText() {
    std::string reply;
    if (released_.load()) {
        reply = "Bluetooth đang tắt để nhường RAM, cần bật rồi khởi động lại mới dùng được.";
    } else if (!enabled_.load()) {
        reply = "Bluetooth đã sẵn sàng nhưng chưa bật sóng.";
    } else if (!connected_.load()) {
        reply = "Bluetooth đang bật nhưng chưa kết nối.";
    } else {
        reply = "Đang kết nối với " + PeerName() + (streaming_.load() ? ", đang phát." : ".");
    }
    switch (route_.load()) {
        case Route::kSpeaker:
            reply += " Mọi tiếng ra loa nhỏ.";
            break;
        case Route::kMusicOnly:
            reply += " Nhạc sang Bluetooth, giọng trợ lý ở loa nhỏ.";
            break;
        case Route::kEverything:
            reply += " Cả nhạc và giọng trợ lý sang Bluetooth.";
            break;
    }
    return reply;
}

std::string BtA2dpSource::SetRoute(const std::string& mode) {
    if (mode == "speaker" || mode == "loa") {
        route_.store(Route::kSpeaker);
    } else if (mode == "all" || mode == "tat_ca") {
        route_.store(Route::kEverything);
    } else if (mode == "music" || mode == "nhac") {
        route_.store(Route::kMusicOnly);
    } else {
        return "Chỉ nhận: speaker, music, all.";
    }
    FlushRing();
    return StatusText();
}

bool BtA2dpSource::TakesAudio(bool is_media) const {
    if (!streaming_.load()) {
        return false;
    }
    switch (route_.load()) {
        case Route::kSpeaker:
            return false;
        case Route::kMusicOnly:
            return is_media;
        case Route::kEverything:
            return true;
    }
    return false;
}

bool BtA2dpSource::OpenResampler(int source_rate) {
    if (resampler_ != nullptr && resampler_source_rate_ == source_rate) {
        return true;
    }
    if (resampler_ != nullptr) {
        esp_ae_rate_cvt_close(resampler_);
        resampler_ = nullptr;
    }
    esp_ae_rate_cvt_cfg_t config = {
        .src_rate = static_cast<uint32_t>(source_rate),
        .dest_rate = static_cast<uint32_t>(kSinkSampleRate),
        .channel = 1,
        .bits_per_sample = ESP_AE_BIT16,
        .complexity = 2,
        .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
    };
    auto ret = esp_ae_rate_cvt_open(&config, &resampler_);
    if (resampler_ == nullptr) {
        ESP_LOGE(TAG, "Failed to open the %d to %d resampler, error code: %d", source_rate,
                 kSinkSampleRate, ret);
        return false;
    }
    resampler_source_rate_ = source_rate;
    ESP_LOGI(TAG, "Resampling %d to %d for Bluetooth", source_rate, kSinkSampleRate);
    return true;
}

void BtA2dpSource::WritePcm(const int16_t* samples, size_t sample_count, int sample_rate) {
    if (ring_ == nullptr || samples == nullptr || sample_count == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(write_mutex_);

    const int16_t* mono = samples;
    size_t mono_count = sample_count;
    if (sample_rate != kSinkSampleRate) {
        if (!OpenResampler(sample_rate)) {
            return;
        }
        uint32_t capacity = 0;
        esp_ae_rate_cvt_get_max_out_sample_num(resampler_, sample_count, &capacity);
        resampled_.resize(capacity);
        uint32_t produced = capacity;
        auto ret = esp_ae_rate_cvt_process(resampler_, (esp_ae_sample_t)samples, sample_count,
                                           (esp_ae_sample_t)resampled_.data(), &produced);
        if (ret != ESP_AE_ERR_OK) {
            ESP_LOGW(TAG, "Resampling failed, error code: %d", ret);
            return;
        }
        mono = resampled_.data();
        mono_count = produced;
    }

    interleaved_.resize(mono_count * kSinkChannels);
    for (size_t i = 0; i < mono_count; i++) {
        interleaved_[i * 2] = mono[i];
        interleaved_[i * 2 + 1] = mono[i];
    }

    auto sent = xRingbufferSend(ring_, interleaved_.data(), interleaved_.size() * sizeof(int16_t),
                                pdMS_TO_TICKS(kWriteTimeoutMs));
    if (sent != pdTRUE) {
        ESP_LOGW(TAG, "Dropped a frame, the sink is not draining the buffer");
    }
}

int32_t BtA2dpSource::FillSinkBuffer(uint8_t* buffer, int32_t size) {
    if (size < 0) {  // the stack is telling us to drop whatever is buffered
        FlushRing();
        return 0;
    }
    if (buffer == nullptr || size == 0 || ring_ == nullptr) {
        return 0;
    }

    int32_t filled = 0;
    while (filled < size) {
        size_t received = 0;
        auto* chunk = static_cast<uint8_t*>(
            xRingbufferReceiveUpTo(ring_, &received, 0, static_cast<size_t>(size - filled)));
        if (chunk == nullptr) {
            break;
        }
        memcpy(buffer + filled, chunk, received);
        vRingbufferReturnItem(ring_, chunk);
        filled += static_cast<int32_t>(received);
    }
    // Silence beats a stutter, and it keeps the sink from dropping the link while idle.
    if (filled < size) {
        memset(buffer + filled, 0, static_cast<size_t>(size - filled));
    }
    return size;
}

void BtA2dpSource::FlushRing() {
    if (ring_ == nullptr) {
        return;
    }
    while (true) {
        size_t received = 0;
        auto* chunk = xRingbufferReceiveUpTo(ring_, &received, 0, kRingBytes);
        if (chunk == nullptr) {
            break;
        }
        vRingbufferReturnItem(ring_, chunk);
    }
}

void BtA2dpSource::RememberSink() {
    std::string address;
    std::string name;
    {
        std::lock_guard<std::mutex> lock(peer_mutex_);
        address = AddressText(peer_address_);
        name = peer_name_;
    }
    Settings settings("bt", true);
    settings.SetString("sink_addr", address);
    settings.SetString("sink_name", name);
}

bool BtA2dpSource::LoadRememberedSink(esp_bd_addr_t address, std::string& name) {
    Settings settings("bt");
    auto text = settings.GetString("sink_addr");
    if (text.size() != 17) {
        return false;
    }
    unsigned int bytes[6];
    if (sscanf(text.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x", &bytes[0], &bytes[1], &bytes[2],
               &bytes[3], &bytes[4], &bytes[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        address[i] = static_cast<uint8_t>(bytes[i]);
    }
    name = settings.GetString("sink_name");
    if (name.empty()) {
        name = text;
    }
    return true;
}

void BtA2dpSource::OnGapEvent(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
    switch (event) {
        case ESP_BT_GAP_DISC_RES_EVT: {
            if (!IsAudioDevice(param)) {
                return;
            }
            auto name = DeviceNameOf(param);
            if (name.empty()) {
                name = AddressText(param->disc_res.bda);
            }
            {
                std::lock_guard<std::mutex> lock(devices_mutex_);
                for (auto& known : devices_) {
                    if (memcmp(known.address, param->disc_res.bda, sizeof(esp_bd_addr_t)) == 0) {
                        if (known.name.size() < name.size()) {
                            known.name = name;
                        }
                        return;
                    }
                }
                Device device;
                memcpy(device.address, param->disc_res.bda, sizeof(esp_bd_addr_t));
                device.name = name;
                devices_.push_back(device);
            }
            ESP_LOGI(TAG, "Found %s (%s)", name.c_str(),
                     AddressText(param->disc_res.bda).c_str());
            if (!connected_.load() && NameMatches(name, wanted_name_)) {
                esp_bt_gap_cancel_discovery();
                ConnectTo(param->disc_res.bda, name);
            }
            break;
        }
        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
            scanning_.store(param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED);
            break;
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "Paired with %s", param->auth_cmpl.device_name);
            } else {
                ESP_LOGE(TAG, "Pairing failed, status %d", param->auth_cmpl.stat);
            }
            break;
#if CONFIG_BT_SSP_ENABLED
        case ESP_BT_GAP_CFM_REQ_EVT:
            ESP_LOGI(TAG, "Accepting pairing, passkey %" PRIu32, param->cfm_req.num_val);
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
            break;
        case ESP_BT_GAP_KEY_NOTIF_EVT:
            ESP_LOGI(TAG, "Passkey is %" PRIu32, param->key_notif.passkey);
            break;
#endif
        default:
            break;
    }
}

void BtA2dpSource::OnA2dpEvent(esp_a2d_cb_event_t event, esp_a2d_cb_param_t* param) {
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT:
            if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                std::string name;
                {
                    std::lock_guard<std::mutex> lock(peer_mutex_);
                    memcpy(peer_address_, param->conn_stat.remote_bda, sizeof(esp_bd_addr_t));
                    if (peer_name_.empty()) {
                        peer_name_ = AddressText(peer_address_);
                    }
                    name = peer_name_;
                }
                connected_.store(true);
                // This runs on the Bluetooth task with a 3 KB stack, so let a task of its
                // own do the NVS write.
                xTaskCreate(
                    [](void*) {
                        GetInstance().RememberSink();
                        vTaskDelete(nullptr);
                    },
                    "bt_remember", 3072, nullptr, 1, nullptr);
                ESP_LOGI(TAG, "Connected to %s", name.c_str());
                FlushRing();
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
            } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                connected_.store(false);
                streaming_.store(false);
                FlushRing();
                ESP_LOGI(TAG, "Disconnected from %s", PeerName().c_str());
            }
            break;
        case ESP_A2D_AUDIO_STATE_EVT:
            if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
                streaming_.store(true);
                ESP_LOGI(TAG, "Streaming started");
            } else {
                streaming_.store(false);
                ESP_LOGI(TAG, "Streaming stopped, state %d", param->audio_stat.state);
            }
            break;
        case ESP_A2D_MEDIA_CTRL_ACK_EVT:
            ESP_LOGI(TAG, "Media control %d acknowledged with status %d", param->media_ctrl_stat.cmd,
                     param->media_ctrl_stat.status);
            break;
        case ESP_A2D_PROF_STATE_EVT:
            ESP_LOGI(TAG, "A2DP profile state %d", param->a2d_prof_stat.init_state);
            break;
        default:
            break;
    }
}

#endif  // CONFIG_BT_A2DP_ENABLE
