#include "SpeakerPlayback.h"
#include "common/AppLogger.h"
#include "esp_timer.h"
#include <cstring>
#include <cstdlib>

// Defines + registers the SPK_RX_BUF ring buffer with BufferManager
DEFINE_BUFFER(SPK_RX_BUF, "spk_rx", 1024 * 1024)



#include "common/thread_config.h"
#include "common/sysdb/EmbeddedSysDb.h"

void SpeakerPlaybackTask::start(esp_codec_dev_handle_t device) {
  this->m_device            = device;
  this->m_is_prebuffering   = true;
  this->m_consecutive_empty = 0;
  TaskBase::start();
}

void SpeakerPlaybackTask::stop() {
  if (m_task_handle != nullptr) {
    m_running = false;
    while (m_task_handle != nullptr) {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
}

void SpeakerPlaybackTask::run() {
  uint32_t sample_rate = EmbeddedSysDb::getInstance().snapshot().audio.sample_rate;
  esp_codec_dev_handle_t device = m_device;
  if (device == nullptr) {
    LOGE_HAL("SpeakerPlaybackTask started without a valid codec device!");
    m_running = false;
    return;
  }

  LOGI_HAL("Speaker Audio Pipeline Active (%lu Hz).", (unsigned long)sample_rate);

  int16_t *dma_safe_buffer = (int16_t *)heap_caps_malloc(
      MAX_AUDIO_CHUNK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  int32_t *expanded_buffer = (int32_t *)heap_caps_malloc(
      EXPANDED_BUF_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  int32_t *silence_buffer = (int32_t *)heap_caps_malloc(
      SILENCE_SAMPLES * 2 * sizeof(int32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  if (!dma_safe_buffer || !expanded_buffer || !silence_buffer) {
    LOGE_HAL("Failed to allocate SpeakerPlayback buffers!");
    if (dma_safe_buffer) heap_caps_free(dma_safe_buffer);
    if (expanded_buffer) heap_caps_free(expanded_buffer);
    if (silence_buffer) heap_caps_free(silence_buffer);
    m_running = false;
    return;
  }
  memset(silence_buffer, 0, SILENCE_SAMPLES * 2 * sizeof(int32_t));

  while (m_running) {
    processPlayback(dma_safe_buffer, expanded_buffer, silence_buffer);
  }

  LOGI_HAL("SpeakerPlaybackTask exiting.");
  heap_caps_free(silence_buffer);
  heap_caps_free(dma_safe_buffer);
  heap_caps_free(expanded_buffer);
}

bool SpeakerPlaybackTask::processPlayback(int16_t* dma_safe_buffer, int32_t* expanded_buffer, int32_t* silence_buffer) {
  auto &bm = BufferManager::getInstance();

  if (!m_hw_valid) {
    m_is_prebuffering = true; // Reset prebuffering on hw pause
    m_consecutive_empty = 0;
    vTaskDelay(pdMS_TO_TICKS(10));
    return false;
  }

  if (m_is_prebuffering) {
    size_t fill_bytes = bm.getUsedBytes(Buffers::SPK_RX_BUF);
    bool assistant_speaking = EmbeddedSysDb::getInstance().assistantSpeaking();
    size_t threshold = assistant_speaking ? PREBUFFER_THRESHOLD : 320;
    if (fill_bytes < threshold) {
      vTaskDelay(pdMS_TO_TICKS(20));
      return false;
    }
    m_is_prebuffering = false;
  }

  size_t rx_chunk_bytes = 0;
  void *rx_data_ptr = bm.receive(Buffers::SPK_RX_BUF, &rx_chunk_bytes,
                                 pdMS_TO_TICKS(20), MAX_AUDIO_CHUNK_BYTES);

  if (rx_data_ptr == nullptr || rx_chunk_bytes == 0) {
    // Write silence to keep the codec DMA clock running — never re-enter
    // prebuffering here; that caused the gibberish on long 30s+ responses.
    // A real end-of-stream is detected by turn_complete + sustained empty.
    m_consecutive_empty++;
    esp_codec_dev_write(m_device, silence_buffer,
                        SILENCE_SAMPLES * 2 * sizeof(int32_t));

    // Reactive check: if turn complete is pending and buffer just emptied
    if (EmbeddedSysDb::getInstance().turnCompletePending()) {
        EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
            s.audio.turn_complete_pending = false;
            s.audio.assistant_speaking    = false;
        });
    }

    // Only re-enter prebuffering after sustained silence (not momentary
    // bursts)
    if (m_consecutive_empty >= EMPTY_THRESHOLD) {
      m_is_prebuffering = true;
      m_consecutive_empty = 0;
    }
    return false;
  }
  m_consecutive_empty = 0; // Reset on every real audio chunk

  size_t num_samples = rx_chunk_bytes / sizeof(int16_t);
  memcpy(dma_safe_buffer, rx_data_ptr, rx_chunk_bytes);
  bm.returnItem(Buffers::SPK_RX_BUF, rx_data_ptr);

  // Expand 16-bit mono → 32-bit stereo (left-justify in 32-bit word)
  for (size_t i = 0; i < num_samples; i++) {
    int32_t sample = ((int32_t)dma_safe_buffer[i]) << 16;
    expanded_buffer[2 * i + 0] = sample; // L
    expanded_buffer[2 * i + 1] = sample; // R
  }

  esp_codec_dev_write(m_device, expanded_buffer,
                      num_samples * 2 * sizeof(int32_t));
  return true;
}
