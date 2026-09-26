#pragma once

#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app/audio/recording/IRecordingEncoder.h"
#include "sd_storage/File.h"

/**
 * @brief Audio recording to the SD card as Ogg Opus files in /sdcard/recordings.
 *
 * Two mutually exclusive modes, selected at start:
 *   STEREO     — the two mics before AFE processing, resampled from the
 *                32 kHz feed to 24 kHz at 48 kbps, or to 16 kHz at 32 kbps
 *                if music is playing when the recording starts (CPU).
 *   PROCESSED  — the AFE's processed mono output (what WakeNet scores),
 *                16 kHz, 24 kbps.
 *
 * Suppresses wake-word detection for the duration (via
 * WakeWordEngine::setWakeWordSuppressed) but leaves playback untouched.
 * Auto-stops after a 10 minute cap. Singleton, called from KeyService and
 * the HTTP API.
 */
class AudioRecorder {
public:
    enum class RecordMode { STEREO, PROCESSED };
    enum class StopReason { MANUAL, AUTO_CAP };

    static AudioRecorder& getInstance();

    /** Returns false (no-op, logs) if a recording is already active. */
    bool startRecording(RecordMode mode);
    void stopRecording(StopReason reason);
    bool isRecording() const { return m_active; }
    const char* activePath() const { return m_active_path; }
    uint32_t encodeRate() const { return m_encode_rate; }

private:
    AudioRecorder() = default;
    AudioRecorder(const AudioRecorder&) = delete;
    AudioRecorder& operator=(const AudioRecorder&) = delete;

    static void writerTaskThunk(void* arg);
    void runWriterTaskLoop();

    bool resolveFilePath(RecordMode mode, uint32_t encode_rate, char* out_path, size_t out_len);

    volatile bool     m_active        = false;
    RecordMode        m_mode          = RecordMode::STEREO;
    sd_storage::File  m_file;
    IRecordingEncoder* m_encoder      = nullptr;
    TaskHandle_t      m_writer_task   = nullptr;
    volatile bool     m_writer_running = false;
    TickType_t        m_deadline_ticks = 0;
    char              m_active_path[160] = {0};

    // Gap detection / silence-padding state (writer task only).
    uint32_t          m_next_expected_seq  = 0;
    int16_t*          m_silence_buf        = nullptr;
    size_t            m_silence_buf_frames = 0;
    uint32_t          m_padded_chunk_count = 0;
    int               m_channels           = 1; // set once at startRecording, used by writer task
    uint32_t          m_encode_rate        = 0;
    TickType_t        m_started_ticks      = 0;

    static constexpr const char* TAG = "AudioRecorder";
    static constexpr uint32_t MAX_DURATION_MS = 10 * 60 * 1000;
    // Opus keeps most scratch on a PSRAM pseudostack, but the SILK (speech)
    // encoder still puts large arrays on the real stack: 12 KB overflowed.
    // PSRAM, so no internal RAM cost.
    static constexpr uint32_t WRITER_STACK = 32 * 1024;

};
