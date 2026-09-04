#include "AudioEngine.h"
#include "AudioDecoderFactory.h"
#include "BufferManager.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"
#include <cstdio>
#include <cmath>

static const char* TAG = "AudioEngine";

AudioEngine::AudioEngine(BufferManager::BufferId rawOpusInId, BufferManager::BufferId pcmOutId)
    : _bm(BufferManager::getInstance()), _rawOpusInId(rawOpusInId), _pcmOutId(pcmOutId) {}

AudioEngine::~AudioEngine() {
    stop();
    // Wait for task to exit
    while (_decoderTaskHandle != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (_pcm_buffer) {
        heap_caps_free(_pcm_buffer);
        _pcm_buffer = nullptr;
    }
    if (_resample_buffer) {
        heap_caps_free(_resample_buffer);
        _resample_buffer = nullptr;
    }
    if (_eventGroup) {
        vEventGroupDelete(_eventGroup);
        _eventGroup = nullptr;
    }
}

bool AudioEngine::initialize(int sampleRate, int channels) {
    _sampleRate = sampleRate;
    _channels = channels;
    _decoder.reset();
    _decoderIdentified = false;

    // Create FreeRTOS EventGroup for zero-CPU task suspension
    if (!_eventGroup) {
        _eventGroup = xEventGroupCreate();
    }
    if (_eventGroup) {
        xEventGroupSetBits(_eventGroup, ENGINE_RUNNING_BIT);
    }

    // Pre-allocate PCM and Resample buffers in PSRAM
    if (!_pcm_buffer) {
        _pcm_buffer = (int16_t*)heap_caps_malloc(_pcm_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!_resample_buffer) {
        _resample_buffer = (int16_t*)heap_caps_malloc(_resample_buffer_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    
    return (_pcm_buffer && _resample_buffer && _eventGroup);
}

void AudioEngine::decodeAndPlayChunk(const uint8_t* payload_data, size_t payload_len, size_t& bytes_consumed) {
    bytes_consumed = 0;
    if (!payload_data || payload_len == 0) return;

    if (!_decoder || !_decoderIdentified) {
        _decoder = AudioDecoderFactory::createDecoder(payload_data, payload_len);
        if (_decoder) {
            _decoder->init(_sampleRate, _channels);
            _decoderIdentified = true;
            ESP_LOGI(TAG, "Initialized decoder strategy: %s", _decoder->getName());
        }
    }

    if (!_decoder) {
        bytes_consumed = payload_len; // Drop if no decoder
        return;
    }

    size_t samples_decoded = 0;
    size_t max_samples = _pcm_buffer_size / sizeof(int16_t);
    DecodeResult result = _decoder->decode(
        payload_data, payload_len,
        _pcm_buffer, max_samples,
        bytes_consumed, samples_decoded
    );

    if (result == DecodeResult::OUTPUT_BUFFER_FULL) {
        ESP_LOGE(TAG, "PCM buffer too small for decoder.");
        return;
    } else if (result == DecodeResult::ERROR_INVALID_STREAM || result == DecodeResult::ERROR_DECODE_FAILED) {
        ESP_LOGD(TAG, "Decoder error result: %d", (int)result);
        if (bytes_consumed == 0) bytes_consumed = payload_len;
        return;
    }

    // Process and output PCM samples
    if (samples_decoded > 0) {
        uint8_t src_channels = _decoder->getSourceChannels();
        int16_t* pcm_mono = _pcm_buffer;
        size_t mono_samples = samples_decoded;

        // Stereo to Mono downmix in-place
        if (src_channels == 2) {
            mono_samples = samples_decoded / 2;
            for (size_t i = 0; i < mono_samples; ++i) {
                int32_t mix = (static_cast<int32_t>(_pcm_buffer[2 * i]) +
                               static_cast<int32_t>(_pcm_buffer[2 * i + 1])) / 2;
                _pcm_buffer[i] = static_cast<int16_t>(mix);
            }
        }

        // Resample mono to target rate (typically 32 kHz)
        uint32_t src_rate = _decoder->getSourceSampleRate();
        uint32_t dst_rate = _sampleRate;

        size_t resampled_count = static_cast<size_t>(mono_samples * dst_rate / src_rate);
        if (resampled_count > _resample_buffer_samples) {
            ESP_LOGE(TAG, "Resample buffer too small (%zu samples), required %zu.",
                     _resample_buffer_samples, resampled_count);
            return;
        }

        float ratio = static_cast<float>(src_rate) / dst_rate;
        for (size_t j = 0; j < resampled_count; ++j) {
            float src_pos = j * ratio;
            size_t idx = static_cast<size_t>(src_pos);
            float frac = src_pos - idx;
            if (idx + 1 < mono_samples) {
                float s0 = pcm_mono[idx];
                float s1 = pcm_mono[idx + 1];
                _resample_buffer[j] = static_cast<int16_t>(s0 + frac * (s1 - s0));
            } else {
                _resample_buffer[j] = pcm_mono[idx];
            }
        }

        // Push final PCM to SPK_RX_BUF with backpressure throttling
        size_t send_bytes = resampled_count * sizeof(int16_t);
        bool sent = _bm.send(_pcmOutId, _resample_buffer, send_bytes, pdMS_TO_TICKS(100));
        if (!sent) {
            ESP_LOGW(TAG, "Failed to send %zu PCM bytes to output buffer", send_bytes);
        } else {
            ESP_LOGD(TAG, "Decoded chunk: consumed=%zu, samples=%zu, pcm_out=%zu",
                     bytes_consumed, samples_decoded, send_bytes);
        }
    }
}

void AudioEngine::runDecodeLoop() {
    ESP_LOGI(TAG, "AudioEngine decode task started");
    _isPlaying = true;
    _decoderIdentified = false;
    if (_decoder) _decoder->reset();

    AudioChunkHeader* current_chunk = nullptr;
    size_t current_offset = 0;
    uint32_t frames_since_yield = 0;

    while (_isPlaying) {
        // Zero-CPU overhead task gating using FreeRTOS EventGroup
        if (_isPaused && _isPlaying) {
            frames_since_yield = 0;
            if (_eventGroup) {
                xEventGroupWaitBits(_eventGroup, ENGINE_RUNNING_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
            }
            continue;
        }

        // 1. Get the next chunk from the ring buffer if we don't have one
        if (current_chunk == nullptr) {
            size_t rx_bytes = 0;
            void* rx_ptr = _bm.receive(_rawOpusInId, &rx_bytes, portMAX_DELAY);
            if (rx_ptr == nullptr) {
                if (!_isPlaying) break;
                continue;
            }

            current_chunk = reinterpret_cast<AudioChunkHeader*>(rx_ptr);
            current_offset = 0;

            if (current_chunk->type == ChunkType::EOF_STREAM) {
                ESP_LOGI(TAG, "Received EOF marker in stream.");
                _bm.returnItem(_rawOpusInId, rx_ptr);
                current_chunk = nullptr;
                break;
            } else if (current_chunk->type == ChunkType::ERROR) {
                ESP_LOGE(TAG, "Received ERROR marker in stream. Aborting playback.");
                _bm.returnItem(_rawOpusInId, rx_ptr);
                current_chunk = nullptr;
                break;
            }
        }

        uint8_t* payload_data = reinterpret_cast<uint8_t*>(current_chunk) + sizeof(AudioChunkHeader);
        size_t payload_len = current_chunk->size;

        size_t bytes_consumed = 0;
        decodeAndPlayChunk(payload_data + current_offset, payload_len - current_offset, bytes_consumed);

        if (bytes_consumed > 0) {
            current_offset += bytes_consumed;
        }

        // If we have fully consumed this chunk, return it to release memory
        if (current_offset >= payload_len) {
            _bm.returnItem(_rawOpusInId, current_chunk);
            current_chunk = nullptr;
        } else if (bytes_consumed == 0) {
            // Decoder is working through buffered audio; yield briefly to avoid busy loop
            vTaskDelay(pdMS_TO_TICKS(2));
        }

        // Yield CPU periodically to prevent task watchdog starvation on CPU 0
        if (++frames_since_yield >= 10) {
            vTaskDelay(2);
            frames_since_yield = 0;
        }
    }

    // Cleanup any partially processed chunk
    if (current_chunk != nullptr) {
        _bm.returnItem(_rawOpusInId, current_chunk);
        current_chunk = nullptr;
    }

    _isPlaying = false;
    _decoderIdentified = false;
    if (_decoder) _decoder->reset();
    ESP_LOGI(TAG, "AudioEngine decode task exiting");
}

void AudioEngine::start() {
    if (_decoderTaskHandle != nullptr) {
        return; // Already running
    }
    _isPlaying = true;
    _isPaused = false;
    _decoderIdentified = false;
    if (_eventGroup) {
        xEventGroupSetBits(_eventGroup, ENGINE_RUNNING_BIT);
    }
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        decoderTaskThunk, "OpusEngine", 8192, this, 5, &_decoderTaskHandle, 0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (ret != pdPASS) {
        xTaskCreatePinnedToCore(decoderTaskThunk, "OpusEngine", 8192, this, 5, &_decoderTaskHandle, 0);
    }
}

void AudioEngine::pause() { 
    _isPaused = true; 
    if (_eventGroup) {
        xEventGroupClearBits(_eventGroup, ENGINE_RUNNING_BIT);
    }
}

void AudioEngine::resume() { 
    _isPaused = false; 
    if (_eventGroup) {
        xEventGroupSetBits(_eventGroup, ENGINE_RUNNING_BIT);
    }
}

void AudioEngine::stop() { 
    _isPlaying = false; 
    _decoderIdentified = false;
    if (_eventGroup) {
        // Clear pause status and unblock task if it was waiting so it can exit cleanly
        xEventGroupSetBits(_eventGroup, ENGINE_RUNNING_BIT);
    }
}

void AudioEngine::decoderTaskThunk(void* pvParameters) {
    static_cast<AudioEngine*>(pvParameters)->runDecodeLoop();
    static_cast<AudioEngine*>(pvParameters)->_decoderTaskHandle = nullptr;
    vTaskDelete(NULL);
}
