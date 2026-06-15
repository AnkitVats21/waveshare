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
  this->m_handle = handle;
  TaskBase::start();
}

void MicCaptureTask::run() {
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
    return;
  }

  LOGI_HAL("MicCapture: reading 4-ch RMNM, forwarding ch0 (primary mic) to ring buffer.");

  while (this->m_running) {
    if (!m_is_enabled) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    processCapture(raw_buffer, pcm_buffer, RAW_BYTES, CHUNK_BYTE_SIZE, FEED_CH, SAMPLES_PER_CHUNK);
  }

  heap_caps_free(raw_buffer);
  heap_caps_free(pcm_buffer);
}

bool MicCaptureTask::processCapture(int16_t* raw_buffer, int16_t* pcm_buffer, size_t raw_bytes, size_t chunk_bytes, int feed_ch, size_t samples_per_chunk) {
  // Read raw 4-channel data (is_get_raw_channel = true)
  if (m_hal.getFeedData(/*raw=*/true, raw_buffer, (int)raw_bytes) == ESP_OK) {

    // Extract channel 0 (primary mic, RMNM slot 0 = Reference; slot 1 = Mic1)
    // RMNM: slot0=Ref, slot1=Mic1, slot2=Noise, slot3=Mic2
    // Use slot 1 (Mic1) as the primary voice signal for streaming.
    for (size_t i = 0; i < samples_per_chunk; i++) {
      pcm_buffer[i] = raw_buffer[i * feed_ch + 1]; // Mic1
    }

    // Push mono PCM to the PSRAM streaming ring buffer via BufferManager
    BufferManager::getInstance().send(Buffers::MIC_TX_BUF, pcm_buffer, chunk_bytes);
    BufferManager::getInstance().send(Buffers::RTP_MIC_BUF, pcm_buffer, chunk_bytes);
    return true;
  } else {
    vTaskDelay(pdMS_TO_TICKS(10));
    return false;
  }
}

void MicCaptureTask::stop() {
  if (m_task_handle != nullptr) {
    m_running = false;
    while (m_task_handle != nullptr) {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
}
