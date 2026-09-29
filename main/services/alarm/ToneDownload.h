#pragma once

#include <cstdint>
#include <string>

namespace Services {

// Downloads a YouTube video's audio into the music library without playing
// it, so it can be an alarm tone (a library song id). One download at a
// time, on a worker task: Invidious resolves the stream, which is written
// to /sdcard/music/<id>.webm and added to CatalogDB as saved.
class ToneDownload {
public:
    enum class State : uint8_t { Idle, Resolving, Downloading, Done, Failed };
    struct Status {
        State state = State::Idle;
        std::string id;
        std::string title;
        uint32_t bytes = 0;
        uint32_t total = 0;     // 0 = unknown
        std::string error;
    };

    // Longest download: a tone, not an album.
    static constexpr uint32_t MAX_BYTES = 10 * 1024 * 1024;

    // False (with why) if the id is not a video id or a download is running.
    // Already in the library: Done at once.
    static bool start(const std::string& id, const std::string& title, const std::string& artist,
                      std::string* error = nullptr);
    static Status status();
    static const char* stateName(State s);
};

} // namespace Services
