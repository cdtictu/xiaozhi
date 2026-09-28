#ifndef MUSIC_LIBRARY_H
#define MUSIC_LIBRARY_H

#include <expected>
#include <string>
#include <vector>

// Songs streamed from a small HTTP server on the LAN (~/xiaozhi/music_server.py):
// <CONFIG_MUSIC_SERVER_URL>index.json lists {"name", "file"} entries of Ogg/Opus files.
// Registers the self.music.* MCP tools and, with the Telegram bot, the /nhac command.
class MusicLibrary {
public:
    static MusicLibrary& GetInstance();
    MusicLibrary(const MusicLibrary&) = delete;
    MusicLibrary& operator=(const MusicLibrary&) = delete;

    // Register the MCP tools and Telegram command. Call before the Telegram bot starts.
    void Initialize();

private:
    struct Song {
        std::string name;
        std::string file;
    };

    MusicLibrary() = default;

    std::expected<std::vector<Song>, std::string> FetchSongs();
    std::expected<Song, std::string> FindSong(const std::string& query);
    // Returns the name of the song that will play
    std::expected<std::string, std::string> Play(const std::string& query);
    std::string HandleTelegram(const std::string& args);
    // Asks the music server to download a song the owner sent to the Telegram bot
    std::string AddFromTelegram(const std::string& file_path, const std::string& name);
    // Asks the music server to fetch a song from YouTube
    std::string AddFromYoutube(const std::string& url);
    // Searches YouTube and waits for the download, so the song can play right away
    std::expected<Song, std::string> FetchFromYoutube(const std::string& query);
};

#endif  // MUSIC_LIBRARY_H
