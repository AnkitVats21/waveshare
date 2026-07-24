#include "app/audio/BtSpeakerPlaybackTask.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <cstring>

// Define the BT_SPK_BUF ring buffer
DEFINE_BUFFER(BT_SPK_BUF, "bt_spk", 32 * 1024)

void BtSpeakerPlaybackTask::start(BtSpeakerHal* hal) {
    m_hal = hal;
    TaskBase::start();
}

void BtSpeakerPlaybackTask::stop() {
    if (m_task_handle != nullptr) {
        m_running = false;
        while (m_task_handle != nullptr) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

void BtSpeakerPlaybackTask::run() {
    if (m_hal == nullptr || !m_hal->isInitialized()) {
        ESP_LOGE(TAG, "BtSpeakerPlaybackTask started without a valid BtSpeakerHal!");
        m_running = false;
        return;
    }

    uint32_t sample_rate = m_hal->getSampleRate();
    ESP_LOGI(TAG, "BtSpeakerPlaybackTask active at %lu Hz (Stereo)", (unsigned long)sample_rate);

    int16_t* silence_buf = (int16_t*)heap_caps_malloc(
        MAX_CHUNK_SAMPLES * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (!silence_buf) {
        ESP_LOGE(TAG, "Failed to allocate silence buffer for BtSpeakerPlaybackTask");
        m_running = false;
        return;
    }
    memset(silence_buf, 0, MAX_CHUNK_SAMPLES * 2 * sizeof(int16_t));

    auto& bm = BufferManager::getInstance();
    while (m_running) {
        size_t frame_bytes = (sample_rate * DRAIN_PERIOD_MS / 1000) * 2 * sizeof(int16_t);

        if (m_paused) {
            // Write silence frame to keep I2S DMA clock running
            size_t written = 0;
            m_hal->writePcm(silence_buf, frame_bytes, &written, 10);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        size_t rx_bytes = 0;
        // Wait up to 50ms for PCM data from AudioEngine instead of instantly zero-filling silence on timeout=0
        void* rx_ptr = bm.receive(Buffers::BT_SPK_BUF, &rx_bytes, pdMS_TO_TICKS(50), frame_bytes);

        if (rx_ptr != nullptr && rx_bytes > 0) {
            size_t written = 0;
            m_hal->writePcm(rx_ptr, rx_bytes, &written, portMAX_DELAY);
            bm.returnItem(Buffers::BT_SPK_BUF, rx_ptr);
        } else {
            // Buffer genuinely empty — write silence to keep I2S clock stable for the ESP32 daughter board
            size_t written = 0;
            m_hal->writePcm(silence_buf, frame_bytes, &written, 10);
        }
    }

    heap_caps_free(silence_buf);
    ESP_LOGI(TAG, "BtSpeakerPlaybackTask exiting");
}
