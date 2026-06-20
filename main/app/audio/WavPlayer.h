#pragma once

#include <cstdint>
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

struct WavInfo {
    uint16_t num_channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint32_t data_size;
};

class WavPlayer {
public:
    static WavPlayer& getInstance();

    /**
     * @brief Play a WAV file asynchronously.
     *        This automatically parses the WAV header chunk-by-chunk (metadata safe),
     *        configures the hardware clock, plays the samples, and restores the clock
     *        and re-arms the wake-word engine when done.
     * @param filepath Path to the WAV file on the SD card (e.g. "/sdcard/alarms/chime.wav")
     * @return true if playback started successfully, false otherwise.
     */
    bool playAsync(const char* filepath);

    /**
     * @brief Read metadata details of a WAV file synchronously.
     * @param filepath Path to the WAV file.
     * @param out_info Structured info to fill.
     * @return true if successfully parsed, false otherwise.
     */
    static bool readWavInfo(const char* filepath, WavInfo& out_info);

    /**
     * @brief Stop the active playback immediately.
     */
    void stop();

    /**
     * @brief Check if a WAV file is currently playing.
     */
    bool isPlaying() const { return m_playing; }

private:
    WavPlayer();
    ~WavPlayer();
    WavPlayer(const WavPlayer&) = delete;
    WavPlayer& operator=(const WavPlayer&) = delete;

    static bool readWavInfo(FILE* f, WavInfo& out_info);
    static void playbackTaskBridge(void* pvParameters);
    void playbackTask();

    char m_filepath[64] = {};
    volatile bool m_playing = false;
    volatile bool m_stop_requested = false;
    TaskHandle_t m_task_handle = nullptr;

    WavInfo m_info = {};
    FILE* m_file_handle = nullptr;

    // Prefetch arena in PSRAM
    uint8_t* m_prefetch_buffer = nullptr;
    bool m_is_prefetched = false;
    size_t m_prefetch_offset = 0;

    static constexpr const char* TAG = "WavPlayer";
};
