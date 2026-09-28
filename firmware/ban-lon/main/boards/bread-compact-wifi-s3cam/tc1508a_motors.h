#ifndef TC1508A_MOTORS_H
#define TC1508A_MOTORS_H

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_timer.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#include "mcp_server.h"
#include "sdkconfig.h"

#if CONFIG_TELEGRAM_BOT_ENABLE
#include "telegram/telegram_bot.h"
#endif

// Two DC motors on a TC1508A dual H-bridge (IN1..IN4, no enable pins).
// Per motor: IN_A=PWM, IN_B=0 forward; IN_A=0, IN_B=PWM backward; both low coast.
// Every movement stops by itself after duration_ms, so the robot cannot run away.
class Tc1508aMotors {
public:
    Tc1508aMotors(gpio_num_t left_a, gpio_num_t left_b, gpio_num_t right_a, gpio_num_t right_b) {
        // Timer 0 / channel 0 belong to the backlight and camera XCLK
        const ledc_timer_config_t timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_10_BIT,
            .timer_num = kTimer,
            .freq_hz = kPwmFrequencyHz,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        ESP_ERROR_CHECK(ledc_timer_config(&timer));

        const gpio_num_t pins[4] = {left_a, left_b, right_a, right_b};
        for (int i = 0; i < 4; i++) {
            const ledc_channel_config_t channel = {
                .gpio_num = pins[i],
                .speed_mode = LEDC_LOW_SPEED_MODE,
                .channel = Channel(i),
                .intr_type = LEDC_INTR_DISABLE,
                .timer_sel = kTimer,
                .duty = 0,
                .hpoint = 0,
            };
            ESP_ERROR_CHECK(ledc_channel_config(&channel));
        }

        const esp_timer_create_args_t stop_timer_args = {
            .callback = [](void* arg) { static_cast<Tc1508aMotors*>(arg)->Stop(); },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "motor_stop",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&stop_timer_args, &stop_timer_));

        AddMcpTools();
#if CONFIG_TELEGRAM_BOT_ENABLE
        AddTelegramCommand();
#endif
    }

    // Speed -100..100 per side, negative is backward
    void Drive(int left, int right, int duration_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        esp_timer_stop(stop_timer_);  // Not running is fine
        SetMotor(0, left);
        SetMotor(2, right);
        if (left != 0 || right != 0) {
            duration_ms = std::clamp(duration_ms, kMinDurationMs, kMaxDurationMs);
            esp_timer_start_once(stop_timer_, static_cast<uint64_t>(duration_ms) * 1000);
        }
    }

    void Stop() { Drive(0, 0, 0); }

    // Returns false for an unknown direction
    bool Move(const std::string& direction, int speed, int duration_ms) {
        speed = std::clamp(speed, 0, 100);
        if (direction == "forward") {
            Drive(speed, speed, duration_ms);
        } else if (direction == "backward") {
            Drive(-speed, -speed, duration_ms);
        } else if (direction == "left") {
            Drive(-speed, speed, duration_ms);  // Spin in place
        } else if (direction == "right") {
            Drive(speed, -speed, duration_ms);
        } else if (direction == "stop") {
            Stop();
        } else {
            return false;
        }
        return true;
    }

private:
    static constexpr ledc_timer_t kTimer = LEDC_TIMER_2;
    static constexpr int kFirstChannel = LEDC_CHANNEL_4;
    static constexpr uint32_t kPwmFrequencyHz = 20000;  // Above hearing, away from the mic
    static constexpr uint32_t kMaxDuty = (1 << 10) - 1;
    static constexpr int kDefaultSpeed = 70;
    static constexpr int kMinDurationMs = 100;
    static constexpr int kMaxDurationMs = 5000;

    esp_timer_handle_t stop_timer_ = nullptr;
    std::mutex mutex_;

    static ledc_channel_t Channel(int index) {
        return static_cast<ledc_channel_t>(kFirstChannel + index);
    }

    void SetDuty(int index, uint32_t duty) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, Channel(index), duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, Channel(index));
    }

    void SetMotor(int first_index, int speed) {
        speed = std::clamp(speed, -100, 100);
        uint32_t duty = kMaxDuty * std::abs(speed) / 100;
        SetDuty(first_index, speed > 0 ? duty : 0);
        SetDuty(first_index + 1, speed < 0 ? duty : 0);
    }

    void AddMcpTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool(
            "self.robot.move",
            "Drive the two-wheeled robot. It stops by itself after `duration_ms`.\n"
            "Args:\n"
            "  `direction`: forward, backward, left (turn left in place), right (turn right in "
            "place) or stop\n"
            "  `speed`: motor power in percent; below about 30 the wheels may not turn\n"
            "  `duration_ms`: how long to move, 100-5000",
            PropertyList({Property("direction", kPropertyTypeString),
                          Property("speed", kPropertyTypeInteger, kDefaultSpeed, 0, 100),
                          Property("duration_ms", kPropertyTypeInteger, 1000, kMinDurationMs,
                                   kMaxDurationMs)}),
            [this](const PropertyList& properties) -> ReturnValue {
                return Move(properties["direction"].value<std::string>(),
                            properties["speed"].value<int>(),
                            properties["duration_ms"].value<int>());
            });
        mcp_server.AddTool("self.robot.stop", "Stop both wheels immediately", PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               Stop();
                               return true;
                           });
    }

#if CONFIG_TELEGRAM_BOT_ENABLE
    void AddTelegramCommand() {
        TelegramBot::GetInstance().AddCommand(
            "dichuyen", "Di chuyển: /dichuyen tien|lui|trai|phai|dung [giây] [tốc độ %]",
            [this](const std::string& args) -> std::string {
                char word[16] = {};
                char seconds_text[16] = {};
                int speed = kDefaultSpeed;
                sscanf(args.c_str(), "%15s %15s %d", word, seconds_text, &speed);
                float seconds = seconds_text[0] != '\0' ? strtof(seconds_text, nullptr) : 1.0f;
                int duration_ms = static_cast<int>(seconds * 1000);

                std::string direction = word;
                const char* label = nullptr;
                if (direction == "tien" || direction == "tiến") {
                    direction = "forward";
                    label = "⬆️ Tiến";
                } else if (direction == "lui" || direction == "lùi") {
                    direction = "backward";
                    label = "⬇️ Lùi";
                } else if (direction == "trai" || direction == "trái") {
                    direction = "left";
                    label = "⬅️ Xoay trái";
                } else if (direction == "phai" || direction == "phải") {
                    direction = "right";
                    label = "➡️ Xoay phải";
                } else if (direction == "dung" || direction == "dừng") {
                    Stop();
                    return "⏹ Đã dừng.";
                } else {
                    return "Cách dùng: /dichuyen tien|lui|trai|phai|dung [giây 0.1-5] [tốc độ "
                           "0-100]\nVí dụ: /dichuyen tien 1.5 80";
                }
                duration_ms = std::clamp(duration_ms, kMinDurationMs, kMaxDurationMs);
                speed = std::clamp(speed, 0, 100);
                Move(direction, speed, duration_ms);
                char reply[96];
                snprintf(reply, sizeof(reply), "%s %.1f giây, tốc độ %d%%", label,
                         duration_ms / 1000.0f, speed);
                return reply;
            });
    }
#endif
};

#endif  // TC1508A_MOTORS_H
