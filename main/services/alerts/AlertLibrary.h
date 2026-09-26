#pragma once

#include "audio_core/AlertPlayer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <mutex>
#include <string>

namespace Services {

// Result of decoding one alert file into a clip.
struct AlertDecode {
    std::shared_ptr<AlertClip> clip;
    uint32_t source_rate = 0;
    bool truncated = false;     // longer than AlertClip::MAX_SECONDS
    uint32_t decode_ms = 0;
    std::string error;          // empty on success
};

// Decodes a whole audio file (any format AudioDecoderFactory knows) to mono
// PCM at MIXER_SAMPLE_RATE in PSRAM. Runs the Opus decoder, so call it from a
// task with a large stack.
AlertDecode decodeAlertFile(const char* path);

/**
 * @brief Loads the alert files from the SD card into AlertPlayer.
 *
 * A low-priority task decodes each alert once and swaps the clip into
 * AlertPlayer; until then, and whenever a file is missing or fails to decode,
 * the built-in tone plays. Nothing here runs when an alert plays.
 */
class AlertLibrary {
public:
    static constexpr const char* ALERT_DIR = "/sdcard/media/alert";
    static constexpr size_t MAX_FILE_BYTES = 256 * 1024;

    struct Status {
        std::string source;       // file path; empty for the built-in tone
        uint32_t samples = 0;
        uint32_t decode_ms = 0;
        bool truncated = false;
        std::string error;        // why the file was not used
    };

    static AlertLibrary& getInstance();

    // Starts the loader task and queues every alert.
    bool start();
    // Queues a reload of one alert (ALERT_COUNT reloads all).
    void reload(AlertType type);
    Status status(AlertType type) const;

private:
    AlertLibrary() = default;
    static void taskEntry(void* arg);
    void load(AlertType type);

    QueueHandle_t m_queue = nullptr;
    mutable std::mutex m_mutex;
    Status m_status[ALERT_COUNT];
};

} // namespace Services
