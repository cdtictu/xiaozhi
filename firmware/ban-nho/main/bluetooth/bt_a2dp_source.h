#ifndef BT_A2DP_SOURCE_H
#define BT_A2DP_SOURCE_H

#include <sdkconfig.h>

#if CONFIG_BT_A2DP_ENABLE

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <esp_a2dp_api.h>
#include <esp_ae_rate_cvt.h>
#include <esp_bt_defs.h>
#include <esp_gap_bt_api.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>

// Streams the decoded audio to a Bluetooth speaker or a car head unit (A2DP source).
// Bluetooth Classic only exists on the plain ESP32, so this is compiled out elsewhere.
// Registers the self.bluetooth.* MCP tools.
class BtA2dpSource {
public:
    // Where the audio goes while a sink is connected
    enum class Route {
        kSpeaker,       // keep everything on the local speaker
        kMusicOnly,     // music to Bluetooth, the assistant's voice stays on the speaker
        kEverything,    // music and voice both to Bluetooth
    };

    static BtA2dpSource& GetInstance();
    BtA2dpSource(const BtA2dpSource&) = delete;
    BtA2dpSource& operator=(const BtA2dpSource&) = delete;

    // Registers the MCP tools. Bluetooth is a boot time decision on this chip: with
    // 320 KB of RAM and no PSRAM, keeping the Bluetooth memory reserved leaves too
    // little heap for the rest of the firmware, so a board that is not set up for
    // Bluetooth hands all of it back and behaves exactly like stock Xiaozhi.
    void Initialize();
    // True when the memory was handed back and Bluetooth needs a reboot to come back
    bool released() const { return released_.load(); }

    // Powers up the Bluetooth stack. Safe to call more than once.
    bool Enable();
    std::string Scan(int seconds);
    // An empty name reconnects the remembered sink
    std::string Connect(const std::string& name);
    std::string Disconnect();
    std::string StatusText();
    std::string SetRoute(const std::string& mode);

    // True when this frame belongs to Bluetooth instead of the local speaker
    bool TakesAudio(bool is_media) const;
    // Mono PCM straight from the decoder. Blocks while the sink drains the buffer,
    // which is what paces playback once the local speaker is out of the path.
    void WritePcm(const int16_t* samples, size_t sample_count, int sample_rate);

private:
    struct Device {
        esp_bd_addr_t address;
        std::string name;
    };

    BtA2dpSource() = default;

    static void GapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param);
    static void A2dpCallback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t* param);
    static int32_t DataCallback(uint8_t* buffer, int32_t size);

    void OnGapEvent(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param);
    void OnA2dpEvent(esp_a2d_cb_event_t event, esp_a2d_cb_param_t* param);
    int32_t FillSinkBuffer(uint8_t* buffer, int32_t size);

    bool OpenResampler(int source_rate);
    void FlushRing();
    bool StartDiscovery(int seconds);
    bool ConnectTo(const esp_bd_addr_t address, const std::string& name);
    void RememberSink();
    std::string PeerName() const;
    // Turns Bluetooth on for the next boot and restarts
    std::string EnableForNextBoot();
    std::string DisableForNextBoot();
    // A `bt` command on the USB serial port, so Bluetooth can be driven without the
    // voice pipeline: useful before the device is activated, and for testing.
    void StartConsole();
    bool LoadRememberedSink(esp_bd_addr_t address, std::string& name);

    RingbufHandle_t ring_ = nullptr;
    esp_ae_rate_cvt_handle_t resampler_ = nullptr;
    int resampler_source_rate_ = 0;
    std::vector<int16_t> resampled_;    // mono at the sink's rate
    std::vector<int16_t> interleaved_;  // stereo at the sink's rate

    std::mutex devices_mutex_;
    std::vector<Device> devices_;
    std::mutex write_mutex_;

    std::atomic<bool> released_{false};
    std::atomic<bool> enabled_{false};
    std::atomic<bool> scanning_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> streaming_{false};
    std::atomic<Route> route_{Route::kMusicOnly};

    // Written from the Bluetooth callback task, read from the console and the MCP tools
    mutable std::mutex peer_mutex_;
    esp_bd_addr_t peer_address_{};
    std::string peer_name_;
    // Set while a scan is looking for this name so discovery can connect right away
    std::string wanted_name_;
};

#endif  // CONFIG_BT_A2DP_ENABLE
#endif  // BT_A2DP_SOURCE_H
