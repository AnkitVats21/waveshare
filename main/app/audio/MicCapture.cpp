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
  const size_t CHUNK_BYTE_SIZE   = SAMPLES_PER_CHUNK * sizeof(int16_t);

  // The HAL now delivers 4 interleaved channels (RMNM) per frame.
  // Allocate a 4-ch raw buffer and a separate mono output buffer.
  const int    FEED_CH            = board.getFeedChannel(); // 4
  const size_t RAW_BYTES          = SAMPLES_PER_CHUNK * FEED_CH * sizeof(int16_t);

  int16_t *raw_buffer = (int16_t *)heap_caps_malloc(RAW_BYTES, MALLOC_CAP_SPIRAM);
  int16_t *pcm_buffer = (int16_t *)heap_caps_malloc(
      CHUNK_BYTE_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  if (!raw_buffer || !pcm_buffer) {
    LOGE_HAL("Failed to allocate MicCapture buffers!");
    if (raw_buffer) heap_caps_free(raw_buffer);
    if (pcm_buffer) heap_caps_free(pcm_buffer);
    vTaskDelete(NULL);
    return;
  }

  LOGI_HAL("MicCapture: reading 4-ch RMNM, forwarding ch0 (primary mic) to ring buffer.");

  while (this->m_is_running) {
    if (!m_is_enabled) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // Read raw 4-channel data (is_get_raw_channel = true)
    if (board.getFeedData(/*raw=*/true, raw_buffer, (int)RAW_BYTES) == ESP_OK) {

      // Extract channel 0 (primary mic, RMNM slot 0 = Reference; slot 1 = Mic1)
      // RMNM: slot0=Ref, slot1=Mic1, slot2=Noise, slot3=Mic2
      // Use slot 1 (Mic1) as the primary voice signal for streaming.
      for (size_t i = 0; i < SAMPLES_PER_CHUNK; i++) {
        pcm_buffer[i] = raw_buffer[i * FEED_CH + 1]; // Mic1
      }

      // Push mono PCM to the PSRAM streaming ring buffer
      BaseType_t ret =
          xRingbufferSend(this->m_tx_buffer, pcm_buffer, CHUNK_BYTE_SIZE, 0);
      if (ret != pdTRUE) {
        // Downstream ring buffer full — normal if RTP streamer is busy
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }

  heap_caps_free(raw_buffer);

  // Cleanup upon thread destruction
  heap_caps_free(pcm_buffer);
  m_task_handle = nullptr;
  vTaskDelete(NULL);
}

// Implement a clean shutdown hook in your class definition
void MicCaptureTask::stop() { this->m_is_running = false; }
