#pragma once

#include <cstdio>
#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app/audio/recording/IRecordingEncoder.h"

/**
 * @brief KEY_2-driven audio-recording-to-SD-card feature.
 *
 * Two mutually exclusive modes, selected at start:
 *   RAW        — pre-AFE-processing mic feed, native hw rate (32kHz), 1 or 4
 *                channels per the persisted record_all_mic_channels config.
 *   RESAMPLED  — AFE's fully-processed mono output (what WakeNet scores),
 *                16kHz mono.
 *
 * Suppresses wake-word detection for the duration (via
 * WakeWordEngine::setWakeWordSuppressed) but leaves playback untouched.
 * Auto-stops after a 10 minute cap. Singleton, called from KeyService.
 */
class AudioRecorder {
public:
    enum class RecordMode { RAW, RESAMPLED };
    enum class StopReason { MANUAL, AUTO_CAP };

    static AudioRecorder& getInstance();

    /** Returns false (no-op, logs) if a recording is already active. */
    bool startRecording(RecordMode mode);
    void stopRecording(StopReason reason);
    bool isRecording() const { return m_active; }

private:
    AudioRecorder() = default;
    AudioRecorder(const AudioRecorder&) = delete;
    AudioRecorder& operator=(const AudioRecorder&) = delete;

    static void writerTaskThunk(void* arg);
    void runWriterTaskLoop();

    bool resolveFilePath(RecordMode mode, char* out_path, size_t out_len);

    volatile bool     m_active        = false;
    RecordMode        m_mode          = RecordMode::RAW;
    FILE*             m_stream        = nullptr;
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

    // Write-coalescing stage: batches many small AFE-chunk writes into
    // fewer, larger disk writes (see appendToStage). Sized to match the
    // AUDIO_CHUNK_SIZE convention already used by media_player's StorageManager.
    static constexpr size_t STAGE_BYTES = 32 * 1024;
    uint8_t*          m_stage_buf  = nullptr;
    size_t            m_stage_used = 0;
    void appendToStage(const int16_t* data, size_t frame_count);
    void flushStage();

    static constexpr const char* TAG = "AudioRecorder";
    static constexpr uint32_t MAX_DURATION_MS = 10 * 60 * 1000;
};
