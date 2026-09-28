#ifndef TELEGRAM_BOT_H
#define TELEGRAM_BOT_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cjson_utils.h"

class Http;

// Telegram bot that lets one owner chat monitor and control the device.
// It long-polls the Bot API from its own task. Built-in commands live in a table built by
// RegisterCommands(): add or remove one AddCommand() call to change the command set.
class TelegramBot {
public:
    // Returns the reply text; an empty string sends no reply.
    using CommandHandler = std::function<std::string(const std::string& args)>;
    // Gets the Telegram file path of an audio file the owner sent and a song name for it;
    // returns the reply text.
    using AudioHandler =
        std::function<std::string(const std::string& file_path, const std::string& name)>;

    static TelegramBot& GetInstance();
    TelegramBot(const TelegramBot&) = delete;
    TelegramBot& operator=(const TelegramBot&) = delete;

    // Register an extra command, e.g. from a board constructor. Call before Start().
    // Names: lowercase a-z, 0-9 and '_' only. Handlers run in the bot task.
    void AddCommand(std::string name, std::string description, CommandHandler handler);

    // Handle audio files sent to the bot. Call before Start(); runs in the bot task.
    void SetAudioHandler(AudioHandler handler) { audio_handler_ = std::move(handler); }

    // Start the polling task. Safe to call more than once.
    void Start();

    // Queue one conversation line for forwarding. Safe to call from any task.
    void OnChatMessage(const char* role, const std::string& text);

private:
    struct Command {
        std::string name;
        std::string description;
        CommandHandler handler;
    };

    TelegramBot();
    ~TelegramBot();

    void RegisterCommands();

    void PollTask();
    bool SkipPendingUpdates();
    bool PollUpdates();
    void HandleUpdate(const cJSON* update);
    void HandleAudio(const cJSON* message);
    void RunCommand(const std::string& text);
    void FlushChatLog();

    bool OpenRequest(const char* method, const std::string& content_type, int timeout_ms,
                     const char* body);
    CJsonUniquePtr ReadResponse(const char* method);
    CJsonUniquePtr CallApi(const char* method, const cJSON* params, int timeout_ms);
    bool SendMessage(const std::string& chat_id, std::string text);
    bool SendMessage(std::string text) { return SendMessage(owner_chat_id_, std::move(text)); }
    bool SendPhoto(const std::string& jpeg, const std::string& caption);
    bool PublishCommands();

    std::string HandleHelp();
    std::string HandleStatus();
    std::string HandleNotify(const std::string& message);
    std::string HandlePhoto(const std::string& caption);
    std::string HandleVolume(const std::string& args);
    std::string HandleBrightness(const std::string& args);
    std::string HandleTheme(const std::string& args);
    std::string HandleEmotion(const std::string& args);
    std::string HandleWifi(const std::string& args);
    std::string HandleForward(const std::string& args);
    std::string HandleReboot();

    const std::string owner_chat_id_;
    std::vector<Command> commands_;
    AudioHandler audio_handler_;
    std::unique_ptr<Http> http_;
    int64_t next_update_id_ = 0;

    std::atomic<bool> started_ = false;
    std::atomic<bool> running_ = false;
    std::atomic<bool> forward_chat_ = false;

    std::mutex chat_mutex_;
    std::string chat_log_;
};

#endif  // TELEGRAM_BOT_H
