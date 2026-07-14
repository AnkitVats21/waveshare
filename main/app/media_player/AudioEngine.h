#pragma once

#include "services/BufferManager.h"
#include "micro_opus/ogg_opus_decoder.h"
#include "AudioSource.h" // For ChunkType, AudioChunkHeader
#include "freertos/event_groups.h"

enum AlertType {
    ALERT_WAKE_CONFIRM,
    ALERT_READY_TO_SPEAK,
    ALERT_SESSION_END,
    ALERT_ERROR,
    ALERT_OFFLINE
};

class AudioEngine {
public:
    AudioEngine(BufferManager::BufferId rawOpusInId, BufferManager::BufferId pcmOutId);
    ~AudioEngine();

    /**
     * @brief Initializes the pre-allocated buffers in PSRAM.
     */
    bool initialize(int sampleRate = 32000, int channels = 1);
    
    void start();
    void stop();
    void pause();
    void resume();
    bool isPlaying() const { return _isPlaying; }

    // Alert playback APIs
    void playAlert(AlertType type);
    bool playAlertFile(const char* path);
    void playTone(float freq_hz, int16_t volume, uint32_t duration_ms, uint32_t fade_ms);

private:
    BufferManager& _bm;
    BufferManager::BufferId _rawOpusInId;
    BufferManager::BufferId _pcmOutId;

    // Use micro-opus Ogg parser for file container structures
    micro_opus::OggOpusDecoder _decoder;
    
    // PSRAM Buffers for working data
    int16_t* _pcm_buffer = nullptr;        // Pre-allocated 32KB
    int16_t* _resample_buffer = nullptr;   // Pre-allocated 4096 samples
    size_t _pcm_buffer_size = 32768;
    size_t _resample_buffer_samples = 4096;

    TaskHandle_t _decoderTaskHandle = nullptr;
    volatile bool _isPlaying = false;
    volatile bool _isPaused = false;
    
    int _sampleRate;
    int _channels;

    static void decoderTaskThunk(void* pvParameters);
    void runDecodeLoop();

    // Unified chunk decoder helper
    void decodeAndPlayChunk(const uint8_t* payload_data, size_t payload_len, size_t& bytes_consumed);

    // FreeRTOS EventGroup for zero-CPU task suspension
    EventGroupHandle_t _eventGroup = nullptr;
    static constexpr EventBits_t ENGINE_RUNNING_BIT = 1 << 0;
};
