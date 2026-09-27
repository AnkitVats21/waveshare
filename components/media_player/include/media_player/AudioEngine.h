#pragma once

#include "services/BufferManager.h"
#include "core_sysdb/AudioRates.h"
#include "IAudioDecoder.h"
#include "PlayerTypes.h" // For ChunkType, AudioChunkHeader
#include "freertos/event_groups.h"
#include <memory>

class AudioEngine {
public:
    AudioEngine(BufferManager::BufferId rawOpusInId, BufferManager::BufferId pcmOutId);
    ~AudioEngine();

    /**
     * @brief Initializes the pre-allocated buffers in PSRAM.
     */
    bool initialize(int sampleRate = MIXER_SAMPLE_RATE, int channels = 1);
    
    void start();
    // Starts decoding after the input was moved to a point before targetMs
    // (a seek or a resume). With `head` (the file's first bytes) the decoder
    // is picked from it; without, the current one is kept. Audio before
    // targetMs is dropped. Only while stopped.
    void startAt(uint32_t targetMs, bool paused, const uint8_t* head = nullptr, size_t headLen = 0);
    void stop();
    // Block (bounded) until the decode task has fully exited and released any
    // ring-buffer items it held. Returns true if it stopped within timeoutMs.
    bool waitUntilStopped(uint32_t timeoutMs = 300);
    void pause();
    void resume();
    bool isPlaying() const { return _isPlaying; }

    // What is playing now: the decoder's position minus the decoded audio
    // still waiting in the output ring.
    uint32_t getPositionMs() const;

private:
    BufferManager& _bm;
    BufferManager::BufferId _rawOpusInId;
    BufferManager::BufferId _pcmOutId;

    // Pluggable audio decoder strategy
    std::unique_ptr<IAudioDecoder> _decoder;
    bool _decoderIdentified = false;
    
    // PSRAM Buffers for working data
    int16_t* _pcm_buffer = nullptr;        // Pre-allocated 32KB (16384 samples)
    int16_t* _resample_buffer = nullptr;   // Pre-allocated 32KB (16384 samples)
    size_t _pcm_buffer_size = 32768;
    size_t _resample_buffer_samples = 16384;

    TaskHandle_t _decoderTaskHandle = nullptr;
    volatile bool _isPlaying = false;
    volatile bool _isPaused = false;
    
    int _sampleRate;
    int _channels;

    // startAt: applied to the decoder when the task starts.
    volatile bool _startPending = false;
    uint32_t _startTargetMs = 0;

    bool spawnTask();
    static void decoderTaskThunk(void* pvParameters);
    void runDecodeLoop();

    // Unified chunk decoder helper
    void decodeAndPlayChunk(const uint8_t* payload_data, size_t payload_len, size_t& bytes_consumed);

    // FreeRTOS EventGroup for zero-CPU task suspension
    EventGroupHandle_t _eventGroup = nullptr;
    static constexpr EventBits_t ENGINE_RUNNING_BIT = 1 << 0;
};
