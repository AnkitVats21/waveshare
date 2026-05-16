#include "MicCapture.h"
#include "common/AppLogger.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/Board.h"
#include <cmath> // For std::abs
#include <cstring>
void MicCaptureTask::start(const GlobalSystemSettings &settings,
                           i2s_chan_handle_t handle,
                           RingbufHandle_t tx_ring_buffer) {
  // Fix 1: Eliminate static struct race conditions by saving state directly
  // into the class variables
  this->m_settings = settings;
  this->m_handle = handle;
  this->m_tx_buffer = tx_ring_buffer;
  this->m_is_running = true; // Added state flag to class definition

  // Pass 'this' as the task parameter
  xTaskCreatePinnedToCore(&MicCaptureTask::worker_bridge, "mic_capture_task",
                          settings.audio_stack_size, this,
                          settings.audio_task_priority, &this->m_task_handle,
                          settings.audio_core_id);
}

// C-Compatible Bridge Function
void MicCaptureTask::worker_bridge(void *pvParameters) {
  MicCaptureTask *instance = static_cast<MicCaptureTask *>(pvParameters);
  instance->worker();
}

void MicCaptureTask::worker() {
  Board &board = Board::getInstance();
  const size_t SAMPLES_PER_CHUNK = 320; // 20ms @ 16kHz
  const size_t CHUNK_BYTE_SIZE = SAMPLES_PER_CHUNK * sizeof(int16_t);

  // Allocate a simple internal SRAM buffer for the final mono PCM data
  int16_t *pcm_buffer = (int16_t *)heap_caps_malloc(
      CHUNK_BYTE_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  if (pcm_buffer == nullptr) {
    LOGE_HAL("Failed to allocate local PCM buffer for Mic Task!");
    vTaskDelete(NULL);
    return;
  }

  LOGI_HAL(
      "Live Audio Feed Pipeline Active: Passthrough Enabled (No AEC/LMS).");

  while (this->m_is_running) {
    if (!m_is_enabled) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // Call the updated board function to get a clean mono 16kHz audio chunk
    // directly
    if (board.getFeedData(pcm_buffer, SAMPLES_PER_CHUNK) == ESP_OK) {

      // Send the raw live feed data straight to the PSRAM streaming Ring Buffer
      BaseType_t ret =
          xRingbufferSend(this->m_tx_buffer, pcm_buffer, CHUNK_BYTE_SIZE, 0);
      if (ret != pdTRUE) {
        // Downstream ring buffer full (Normal if RTP network streamer is
        // busy/lagging)
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }

  // Cleanup upon thread destruction
  heap_caps_free(pcm_buffer);
  m_task_handle = nullptr;
  vTaskDelete(NULL);
}

// Implement a clean shutdown hook in your class definition
void MicCaptureTask::stop() { this->m_is_running = false; }
