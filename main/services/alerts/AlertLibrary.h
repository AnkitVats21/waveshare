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
 * Each alert's source, gain and enable come from the "alerts" collection of
 * system.ndb; an alert with no saved config plays <name>.ogg from ALERT_DIR,
 * or the built-in tone when there is no such file.
 *
 * A low-priority task decodes each alert once and swaps the clip into
 * AlertPlayer; until then, and whenever a file is missing or fails to decode,
 * the built-in tone plays. Nothing here runs when an alert plays.
 */
class AlertLibrary {
public:
    static constexpr const char* ALERT_DIR = "/sdcard/media/alert";
    static constexpr size_t MAX_FILE_BYTES = 256 * 1024;

    static constexpr const char* BUILTIN = "builtin";
    static constexpr float MIN_GAIN_DB = -24.0f;
    static constexpr float MAX_GAIN_DB = 6.0f;

    struct Status {
        bool custom = false;      // has a saved config (else all defaults)
        bool enabled = true;
        float gain_db = 0.0f;
        std::string setting;      // saved source: "", "builtin" or a file name
        std::string file;         // file playing; empty for the built-in tone
        uint32_t samples = 0;
        uint32_t decode_ms = 0;
        bool truncated = false;
        std::string error;        // why the configured file is not playing
    };

    // A plain file name in ALERT_DIR: no path separators, not hidden.
    static bool isValidFileName(const std::string& name);

    static AlertLibrary& getInstance();

    // Starts the loader task and queues every alert.
    bool start();
    // Queues a reload of one alert (ALERT_COUNT reloads all).
    void reload(AlertType type);
    // Reloads one alert from its saved config on the loader task and waits
    // for it; false on timeout (the reload still completes later).
    bool reloadAndWait(AlertType type, uint32_t timeout_ms);
    // Decodes a file on the loader task (the caller's stack is too small for
    // the Opus decoder) and waits; false on timeout.
    bool decodeAndWait(const std::string& path, AlertDecode& out, uint32_t timeout_ms);
    // Applies enable and gain immediately, without decoding again.
    void applySettings(AlertType type, bool enabled, float gain_db);
    Status status(AlertType type) const;

private:
    struct Job;

    AlertLibrary() = default;
    static void taskEntry(void* arg);
    bool submit(const std::shared_ptr<Job>& job, uint32_t timeout_ms);
    void load(AlertType type);

    QueueHandle_t m_queue = nullptr;
    mutable std::mutex m_mutex;
    Status m_status[ALERT_COUNT];
};

} // namespace Services
