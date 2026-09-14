#include "audio_core/AlertPlayer.h"
#include "audio_core/AudioOrchestrator.h"
#include "audio_core/SpeakerPlayback.h"
#include "core_sysdb/BufferManager.h"
#include "audio_core/Resampler.h"
#include "core_sysdb/thread_config.h"
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
    if (path && m_file_decoder) {
        custom_played = m_file_decoder->playAlertFile(path);
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
                    constexpr uint32_t GAP_SAMPLES = (44100 * 40) / 1000;
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

void AlertPlayer::playTone(float freq_hz, int16_t volume, uint32_t duration_ms, uint32_t fade_ms) {
    constexpr uint32_t SAMPLE_RATE = 44100;
    const uint32_t total_samples = (SAMPLE_RATE * duration_ms) / 1000;
    const uint32_t fade_samples  = (SAMPLE_RATE * fade_ms) / 1000;

    constexpr uint32_t BLOCK = 882; // 20 ms @ 44.1 kHz
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
