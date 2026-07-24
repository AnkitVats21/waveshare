#include "AudioEngine.h"
#include "BufferManager.h"
#include "app/audio/BtSpeakerPlaybackTask.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
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
    size_t samples_decoded = 0;

    // Decode Ogg/Opus packet to PCM
    micro_opus::OggOpusResult result = _decoder.decode(
        payload_data, payload_len,
        reinterpret_cast<uint8_t*>(_pcm_buffer), _pcm_buffer_size,
        bytes_consumed, samples_decoded
    );

    if (result == micro_opus::OGG_OPUS_OUTPUT_BUFFER_TOO_SMALL) {
        ESP_LOGE(TAG, "PCM buffer too small (%zu bytes), required %zu.",
                 _pcm_buffer_size, _decoder.get_required_output_buffer_size());
        return;
    } else if (result != micro_opus::OGG_OPUS_OK) {
        ESP_LOGE(TAG, "Opus decode error: %d", result);
        return;
    }

    // Process and output PCM samples
    if (samples_decoded > 0) {
        uint8_t channels = _decoder.get_channels();
        uint32_t src_rate = _decoder.get_sample_rate();

        // 1. Send high-quality 48 kHz Stereo PCM to BT Speaker buffer (if initialized)
        if (Buffers::BT_SPK_BUF != BufferManager::INVALID && Buffers::BT_SPK_BUF != _pcmOutId) {
            if (channels == 2 && src_rate == 48000) {
                // Already 48 kHz stereo 16-bit PCM!
                size_t bt_bytes = samples_decoded * 2 * sizeof(int16_t);
                _bm.send(Buffers::BT_SPK_BUF, _pcm_buffer, bt_bytes, pdMS_TO_TICKS(10));
            } else if (channels == 1 && src_rate == 48000) {
                // Mono 48 kHz -> Expand to Stereo 48 kHz using _resample_buffer
                size_t req_samples = samples_decoded * 2;
                if (req_samples <= _resample_buffer_samples) {
                    for (size_t i = 0; i < samples_decoded; ++i) {
                        _resample_buffer[2 * i + 0] = _pcm_buffer[i]; // L
                        _resample_buffer[2 * i + 1] = _pcm_buffer[i]; // R
                    }
                    _bm.send(Buffers::BT_SPK_BUF, _resample_buffer, req_samples * sizeof(int16_t), pdMS_TO_TICKS(10));
                }
            } else {
                // Resample to 48 kHz stereo if src_rate != 48000
                uint32_t bt_dst_rate = 48000;
                size_t resample_frames = static_cast<size_t>(samples_decoded * bt_dst_rate / src_rate);
                size_t req_samples = resample_frames * 2;
                if (req_samples <= _resample_buffer_samples) {
                    float ratio = static_cast<float>(src_rate) / bt_dst_rate;
                    for (size_t j = 0; j < resample_frames; ++j) {
                        float src_pos = j * ratio;
                        size_t idx = static_cast<size_t>(src_pos);
                        float frac = src_pos - idx;
                        if (channels == 2) {
                            int16_t l0 = _pcm_buffer[2 * idx + 0];
                            int16_t r0 = _pcm_buffer[2 * idx + 1];
                            int16_t l1 = (idx + 1 < samples_decoded) ? _pcm_buffer[2 * (idx + 1) + 0] : l0;
                            int16_t r1 = (idx + 1 < samples_decoded) ? _pcm_buffer[2 * (idx + 1) + 1] : r0;
                            _resample_buffer[2 * j + 0] = static_cast<int16_t>(l0 + frac * (l1 - l0));
                            _resample_buffer[2 * j + 1] = static_cast<int16_t>(r0 + frac * (r1 - r0));
                        } else {
                            int16_t s0 = _pcm_buffer[idx];
                            int16_t s1 = (idx + 1 < samples_decoded) ? _pcm_buffer[idx + 1] : s0;
                            int16_t val = static_cast<int16_t>(s0 + frac * (s1 - s0));
                            _resample_buffer[2 * j + 0] = val;
                            _resample_buffer[2 * j + 1] = val;
                        }
                    }
                    _bm.send(Buffers::BT_SPK_BUF, _resample_buffer, req_samples * sizeof(int16_t), pdMS_TO_TICKS(10));
                }
            }
        }

        // 2. Local Speaker: Downmix Stereo to Mono & Resample for onboard codec
        int16_t* pcm_mono = _pcm_buffer;
        size_t mono_samples = samples_decoded;

        if (channels == 2) {
            for (size_t i = 0; i < samples_decoded; ++i) {
                int32_t mix = (static_cast<int32_t>(_pcm_buffer[2 * i]) +
                               static_cast<int32_t>(_pcm_buffer[2 * i + 1])) / 2;
                _pcm_buffer[i] = static_cast<int16_t>(mix);
            }
        }

        uint32_t dst_rate = _sampleRate > 0 ? static_cast<uint32_t>(_sampleRate) : 44100;
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

        size_t send_bytes = resampled_count * sizeof(int16_t);
        bool sent = _bm.send(_pcmOutId, _resample_buffer, send_bytes, pdMS_TO_TICKS(100));
        if (!sent) {
            ESP_LOGW(TAG, "Failed to send %zu PCM bytes to local output buffer", send_bytes);
        } else {
            ESP_LOGD(TAG, "Decoded chunk: consumed=%zu, samples=%zu, pcm_out=%zu",
                     bytes_consumed, samples_decoded, send_bytes);
        }
    }
}

void AudioEngine::runDecodeLoop() {
    ESP_LOGI(TAG, "AudioEngine decode task started");
    _isPlaying = true;
    _decoder.reset();

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
        if (current_offset >= payload_len || bytes_consumed == 0) {
            _bm.returnItem(_rawOpusInId, current_chunk);
            current_chunk = nullptr;
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
    ESP_LOGI(TAG, "AudioEngine decode task exiting");
}

void AudioEngine::start() {
    if (_decoderTaskHandle != nullptr) {
        return; // Already running
    }
    _isPlaying = true;
    _isPaused = false;
    if (_eventGroup) {
        xEventGroupSetBits(_eventGroup, ENGINE_RUNNING_BIT);
    }
    xTaskCreatePinnedToCore(decoderTaskThunk, "OpusEngine", 8192, this, 5, &_decoderTaskHandle, 0);
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

bool AudioEngine::playAlertFile(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;

    ESP_LOGI(TAG, "Playing custom alert Ogg file via AudioEngine: %s", path);

    _decoder.reset();

    // Heap-allocate the read buffer to save stack space during VFS filesystem calls
    constexpr size_t READ_BUF_SIZE = 1024;
    uint8_t* read_buf = (uint8_t*)malloc(READ_BUF_SIZE);
    if (!read_buf) {
        ESP_LOGE(TAG, "Failed to allocate read buffer for alert");
        fclose(f);
        return false;
    }

    size_t payload_len = 0;
    size_t current_offset = 0;

    while (true) {
        if (payload_len == 0 || current_offset >= payload_len) {
            payload_len = fread(read_buf, 1, READ_BUF_SIZE, f);
            current_offset = 0;
            if (payload_len == 0) {
                break; // EOF
            }
        }

        size_t bytes_consumed = 0;
        decodeAndPlayChunk(read_buf + current_offset, payload_len - current_offset, bytes_consumed);

        if (bytes_consumed > 0) {
            current_offset += bytes_consumed;
        } else {
            payload_len = 0; // Force reload if no progress is made
        }
    }

    free(read_buf);
    fclose(f);
    _decoder.reset(); // Reset again so music decoder starts clean
    return true;
}

void AudioEngine::playTone(float freq_hz, int16_t volume, uint32_t duration_ms, uint32_t fade_ms) {
    const uint32_t SAMPLE_RATE = _sampleRate > 0 ? static_cast<uint32_t>(_sampleRate) : 44100;
    const uint32_t total_samples = (SAMPLE_RATE * duration_ms) / 1000;
    const uint32_t fade_samples  = (SAMPLE_RATE * fade_ms) / 1000;
    
    constexpr uint32_t BLOCK = 128;
    int16_t buf[BLOCK];
    
    uint32_t sent = 0;
    while (sent < total_samples) {
        const uint32_t n = (total_samples - sent < BLOCK) ? (total_samples - sent) : BLOCK;
        
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t s = sent + i;
            float env = 1.0f;
            
            if (s < fade_samples && fade_samples > 0) {
                env = (float)s / (float)fade_samples;
            }
            else if (s >= (total_samples - fade_samples) && fade_samples > 0) {
                env = (float)(total_samples - s) / (float)fade_samples;
            }
            
            const float angle = 2.0f * 3.14159265f * freq_hz * (float)s / (float)SAMPLE_RATE;
            buf[i] = (int16_t)(env * (float)volume * sinf(angle));
        }
        
        _bm.send(_pcmOutId, reinterpret_cast<uint8_t*>(buf), n * sizeof(int16_t), pdMS_TO_TICKS(10));
        sent += n;
    }
}

void AudioEngine::playAlert(AlertType type) {
    const char* path = nullptr;
    switch (type) {
        case ALERT_WAKE_CONFIRM:    path = "/sdcard/media/alert/wake_confirm.ogg"; break;
        case ALERT_READY_TO_SPEAK:  path = "/sdcard/media/alert/ready_to_speak.ogg"; break;
        case ALERT_SESSION_END:     path = "/sdcard/media/alert/session_end.ogg"; break;
        case ALERT_ERROR:           path = "/sdcard/media/alert/error.ogg"; break;
        case ALERT_OFFLINE:         path = "/sdcard/media/alert/offline.ogg"; break;
        default: break;
    }
    
    bool custom_played = false;
    if (path) {
        FILE* f = fopen(path, "rb");
        if (f) {
            fclose(f);
            custom_played = playAlertFile(path);
        } else {
            f = fopen("/sdcard/media/alert/alert.ogg", "rb");
            if (f) {
                fclose(f);
                custom_played = playAlertFile("/sdcard/media/alert/alert.ogg");
            }
        }
    }
    
    if (!custom_played) {
        switch (type) {
            case ALERT_WAKE_CONFIRM:
                playTone(784.0f, 8000, 80, 15);   // G5
                playTone(987.8f, 8000, 80, 15);   // B5
                break;
            case ALERT_READY_TO_SPEAK:
                playTone(523.3f, 10000, 90, 15);  // C5
                playTone(659.3f, 10000, 90, 15);  // E5
                playTone(784.0f, 10000, 130, 20); // G5
                break;
            case ALERT_SESSION_END:
                playTone(659.3f, 7000, 100, 20);  // E5
                playTone(523.3f, 7000, 120, 20);  // C5
                break;
            case ALERT_ERROR:
                playTone(440.0f, 9000, 60, 10);
                {
                    constexpr uint32_t GAP_SAMPLES = (32000 * 40) / 1000;
                    int16_t silence[GAP_SAMPLES] = {};
                    _bm.send(_pcmOutId, reinterpret_cast<uint8_t*>(silence), GAP_SAMPLES * sizeof(int16_t), pdMS_TO_TICKS(10));
                }
                playTone(440.0f, 9000, 60, 10);
                break;
            case ALERT_OFFLINE:
                playTone(146.8f, 8000, 120, 25);  // D3
                break;
        }
    }
}
