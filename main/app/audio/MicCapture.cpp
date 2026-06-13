#include "MicCapture.h"
#include "hal/audio/AudioHal.h"
#include "common/AppLogger.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>
#include <cstring>

// Defines + registers the MIC_TX_BUF ring buffer with BufferManager
DEFINE_BUFFER(MIC_TX_BUF, "mic_tx", 128 * 1024)
DEFINE_BUFFER(RTP_MIC_BUF, "rtp_mic", 64 * 1024)

#include "common/thread_config.h"

void MicCaptureTask::start(i2s_chan_handle_t handle) {
  this->m_handle      = handle;
  this->m_is_running  = true;

  // Pass 'this' as the task parameter
  xTaskCreatePinnedToCore(&MicCaptureTask::worker_bridge, "mic_capture_task",
                          8 * 1024, this,
                          ThreadConfig::Priority::MIC_CAPTURE, &this->m_task_handle,
                          ThreadConfig::CORE_AUDIO);
}

// C-Compatible Bridge Function
void MicCaptureTask::worker_bridge(void *pvParameters) {
  MicCaptureTask *instance = static_cast<MicCaptureTask *>(pvParameters);
  instance->worker();
}

void MicCaptureTask::worker() {
  AudioHal &audio = m_hal;
  const size_t SAMPLES_PER_CHUNK = 320; // 20ms @ 16kHz
  const size_t CHUNK_BYTE_SIZE   = SAMPLES_PER_CHUNK * sizeof(int16_t);

  // The HAL now delivers 4 interleaved channels (RMNM) per frame.
  // Allocate a 4-ch raw buffer and a separate mono output buffer.
  const int    FEED_CH            = audio.getFeedChannel(); // 4
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
    if (audio.getFeedData(/*raw=*/true, raw_buffer, (int)RAW_BYTES) == ESP_OK) {

      // Extract channel 0 (primary mic, RMNM slot 0 = Reference; slot 1 = Mic1)
      // RMNM: slot0=Ref, slot1=Mic1, slot2=Noise, slot3=Mic2
      // Use slot 1 (Mic1) as the primary voice signal for streaming.
      for (size_t i = 0; i < SAMPLES_PER_CHUNK; i++) {
        pcm_buffer[i] = raw_buffer[i * FEED_CH + 1]; // Mic1
      }

      // Push mono PCM to the PSRAM streaming ring buffer via BufferManager
      BufferManager::getInstance().send(Buffers::MIC_TX_BUF, pcm_buffer, CHUNK_BYTE_SIZE);
      BufferManager::getInstance().send(Buffers::RTP_MIC_BUF, pcm_buffer, CHUNK_BYTE_SIZE);
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
