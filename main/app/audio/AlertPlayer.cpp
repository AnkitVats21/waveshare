#include "AlertPlayer.h"
#include "AudioOrchestrator.h"
#include "SpeakerPlayback.h"
#include "app/media_player/AudioDecoderFactory.h"
#include "services/BufferManager.h"
#include "services/storage/StorageService.h"
#include "common/thread_config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <cstdio>
#include <cmath>

AlertPlayer& AlertPlayer::getInstance() {
    static AlertPlayer instance;
    return instance;
}

AlertPlayer::AlertPlayer() {
    m_queue = xQueueCreate(4, sizeof(AlertType));
}

AlertPlayer::~AlertPlayer() {
    stop();
    if (m_queue) {
        vQueueDelete(m_queue);
        m_queue = nullptr;
    }
}

bool AlertPlayer::begin() {
    ESP_LOGI(TAG, "AlertPlayer initialized.");
    return true;
}

bool AlertPlayer::start() {
    if (m_task_handle != nullptr) {
        return true;
    }
    m_running = true;
    BaseType_t res = xTaskCreatePinnedToCore(
        workerTaskThunk,
        "alert_player",
        ThreadConfig::StackSize::STACK_LARGE,
        this,
        ThreadConfig::Priority::AUDIO_ALERT,
        &m_task_handle,
        ThreadConfig::CORE_AUDIO
    );
    return (res == pdPASS);
}

void AlertPlayer::stop() {
    if (m_task_handle != nullptr) {
        m_running = false;
        AlertType dummy = ALERT_ERROR;
        xQueueSend(m_queue, &dummy, 0);
        while (m_task_handle != nullptr) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

void AlertPlayer::playAlert(AlertType type) {
    if (m_queue) {
        xQueueSend(m_queue, &type, 0);
    }
}

void AlertPlayer::workerTaskThunk(void* pvParameters) {
    static_cast<AlertPlayer*>(pvParameters)->workerTask();
    vTaskDelete(NULL);
}

void AlertPlayer::workerTask() {
    ESP_LOGI(TAG, "AlertPlayer worker task active.");
    AlertType type;

    while (m_running) {
        if (xQueueReceive(m_queue, &type, portMAX_DELAY) == pdTRUE) {
            if (!m_running) break;

            AudioOrchestrator::getInstance().notifyAlertStarted();
            processAlert(type);

            // Wait for ALERT_RX_BUF to fully drain so the alert finishes cleanly
            auto& bm = BufferManager::getInstance();
            while (bm.getUsedBytes(Buffers::ALERT_RX_BUF) > 0) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            // Small cushion for I2S DMA latency
            vTaskDelay(pdMS_TO_TICKS(40));

            AudioOrchestrator::getInstance().notifyAlertEnded();
        }
    }

    m_task_handle = nullptr;
}

void AlertPlayer::processAlert(AlertType type) {
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
    if (path && Services::StorageService::getInstance().isMounted()) {
        if (Services::StorageService::getInstance().fileExists(path)) {
            custom_played = playAlertFile(path);
        } else if (Services::StorageService::getInstance().fileExists("/sdcard/media/alert/alert.ogg")) {
            custom_played = playAlertFile("/sdcard/media/alert/alert.ogg");
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
                    BufferManager::getInstance().send(Buffers::ALERT_RX_BUF, silence, GAP_SAMPLES * sizeof(int16_t), pdMS_TO_TICKS(10));
                }
                playTone(440.0f, 9000, 60, 10);
                break;
            case ALERT_OFFLINE:
                playTone(146.8f, 8000, 120, 25);  // D3
                break;
        }
    }
}

bool AlertPlayer::playAlertFile(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;

    ESP_LOGI(TAG, "Playing custom alert: %s", path);

    constexpr size_t READ_BUF_SIZE = 1024;
    uint8_t* read_buf = (uint8_t*)malloc(READ_BUF_SIZE);
    constexpr size_t MAX_SAMPLES = 4096;
    int16_t* pcm_buf = (int16_t*)heap_caps_malloc(MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t* resample_buf = (int16_t*)heap_caps_malloc(MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!read_buf || !pcm_buf || !resample_buf) {
        ESP_LOGE(TAG, "Failed to allocate memory for alert decoding");
        if (read_buf) free(read_buf);
        if (pcm_buf) heap_caps_free(pcm_buf);
        if (resample_buf) heap_caps_free(resample_buf);
        fclose(f);
        return false;
    }

    std::unique_ptr<IAudioDecoder> decoder;
    size_t payload_len = 0;
    size_t current_offset = 0;
    auto& bm = BufferManager::getInstance();

    while (true) {
        if (payload_len == 0 || current_offset >= payload_len) {
            payload_len = fread(read_buf, 1, READ_BUF_SIZE, f);
            current_offset = 0;
            if (payload_len == 0) break; // EOF
        }

        if (!decoder) {
            decoder = AudioDecoderFactory::createDecoder(read_buf + current_offset, payload_len - current_offset);
            if (decoder) {
                decoder->init(32000, 1);
            } else {
                break;
            }
        }

        size_t bytes_consumed = 0;
        size_t samples_decoded = 0;
        DecodeResult res = decoder->decode(
            read_buf + current_offset, payload_len - current_offset,
            pcm_buf, MAX_SAMPLES,
            bytes_consumed, samples_decoded
        );

        if (res == DecodeResult::STREAM_END || 
            res == DecodeResult::ERROR_INVALID_STREAM || 
            res == DecodeResult::ERROR_DECODE_FAILED) {
            if (samples_decoded == 0) {
                break;
            }
        }

        if (samples_decoded > 0) {
            uint8_t src_channels = decoder->getSourceChannels();
            int16_t* pcm_mono = pcm_buf;
            size_t mono_samples = samples_decoded;

            if (src_channels == 2) {
                mono_samples = samples_decoded / 2;
                for (size_t i = 0; i < mono_samples; ++i) {
                    int32_t mix = (static_cast<int32_t>(pcm_buf[2 * i]) + static_cast<int32_t>(pcm_buf[2 * i + 1])) / 2;
                    pcm_buf[i] = static_cast<int16_t>(mix);
                }
            }

            uint32_t src_rate = decoder->getSourceSampleRate();
            uint32_t dst_rate = 32000;
            size_t resampled_count = (mono_samples * dst_rate) / src_rate;
            if (resampled_count > MAX_SAMPLES) resampled_count = MAX_SAMPLES;

            float ratio = static_cast<float>(src_rate) / dst_rate;
            for (size_t j = 0; j < resampled_count; ++j) {
                float src_pos = j * ratio;
                size_t idx = static_cast<size_t>(src_pos);
                float frac = src_pos - idx;
                if (idx + 1 < mono_samples) {
                    float s0 = pcm_mono[idx];
                    float s1 = pcm_mono[idx + 1];
                    resample_buf[j] = static_cast<int16_t>(s0 + frac * (s1 - s0));
                } else {
                    resample_buf[j] = pcm_mono[idx];
                }
            }

            bm.send(Buffers::ALERT_RX_BUF, resample_buf, resampled_count * sizeof(int16_t), pdMS_TO_TICKS(100));
        }

        if (bytes_consumed > 0) {
            current_offset += bytes_consumed;
        } else {
            payload_len = 0;
        }
    }

    free(read_buf);
    heap_caps_free(pcm_buf);
    heap_caps_free(resample_buf);
    fclose(f);
    return true;
}

void AlertPlayer::playTone(float freq_hz, int16_t volume, uint32_t duration_ms, uint32_t fade_ms) {
    constexpr uint32_t SAMPLE_RATE = 32000;
    const uint32_t total_samples = (SAMPLE_RATE * duration_ms) / 1000;
    const uint32_t fade_samples  = (SAMPLE_RATE * fade_ms) / 1000;

    constexpr uint32_t BLOCK = 640; // 20 ms @ 32 kHz
    int16_t buf[BLOCK];

    uint32_t sent = 0;
    auto& bm = BufferManager::getInstance();

    while (sent < total_samples) {
        const uint32_t n = (total_samples - sent < BLOCK) ? (total_samples - sent) : BLOCK;
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t s = sent + i;
            float env = 1.0f;
            if (s < fade_samples && fade_samples > 0) {
                env = (float)s / (float)fade_samples;
            } else if (s >= (total_samples - fade_samples) && fade_samples > 0) {
                env = (float)(total_samples - s) / (float)fade_samples;
            }
            const float angle = 2.0f * 3.14159265f * freq_hz * (float)s / (float)SAMPLE_RATE;
            buf[i] = (int16_t)(env * (float)volume * sinf(angle));
        }
        bm.send(Buffers::ALERT_RX_BUF, buf, n * sizeof(int16_t), pdMS_TO_TICKS(50));
        sent += n;
    }
}
