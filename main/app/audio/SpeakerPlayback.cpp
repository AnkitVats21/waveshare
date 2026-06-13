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
  if (m_is_running) return;
  m_is_running = true;

  auto snap = EmbeddedSysDb::getInstance().snapshot();

  TaskParam *param = new TaskParam();
  param->self        = this;
  param->sample_rate = snap.audio.sample_rate;
  param->device      = device;

  xTaskCreatePinnedToCore(&SpeakerPlaybackTask::worker_bridge, "speaker_playback_task", 
                          8 * 1024, param,
                          ThreadConfig::Priority::SPEAKER_PLAYBACK, &m_task_handle,
                          ThreadConfig::CORE_AUDIO);
}

void SpeakerPlaybackTask::stop() {
  if (!m_is_running) return;
  m_is_running = false;
  LOGI_HAL("Requesting SpeakerPlaybackTask stop...");
}

void SpeakerPlaybackTask::worker_bridge(void *pvParameters) {
  TaskParam *param = static_cast<TaskParam *>(pvParameters);
  SpeakerPlaybackTask *self = param->self;
  uint32_t sample_rate = param->sample_rate;
  esp_codec_dev_handle_t device = param->device;
  delete param;
  self->worker(sample_rate, device);
}

void SpeakerPlaybackTask::worker(uint32_t sample_rate,
                                 esp_codec_dev_handle_t device) {
  auto &bm = BufferManager::getInstance();
  if (device == nullptr) {
    LOGE_HAL("SpeakerPlaybackTask started without a valid codec device!");
    m_is_running = false;
    vTaskDelete(NULL);
    return;
  }

  LOGI_HAL("Speaker Audio Pipeline Active (%lu Hz).", (unsigned long)sample_rate);

  // The I2S bus runs in 32-bit stereo mode.  RTP data arrives as 16-bit mono.
  // We expand each int16 sample into a 32-bit left-justified stereo pair
  // before writing to the codec device.
  const size_t MAX_AUDIO_CHUNK_SAMPLES = 1024;  // in int16 samples
  const size_t MAX_AUDIO_CHUNK_BYTES   = MAX_AUDIO_CHUNK_SAMPLES * sizeof(int16_t);
  const size_t EXPANDED_BUF_BYTES      = MAX_AUDIO_CHUNK_SAMPLES * 2 * sizeof(int32_t);

  int16_t *dma_safe_buffer = (int16_t *)heap_caps_malloc(
      MAX_AUDIO_CHUNK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  int32_t *expanded_buffer = (int32_t *)heap_caps_malloc(
      EXPANDED_BUF_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  const size_t SILENCE_SAMPLES = 320;
  int32_t *silence_buffer = (int32_t *)heap_caps_malloc(
      SILENCE_SAMPLES * 2 * sizeof(int32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  memset(silence_buffer, 0, SILENCE_SAMPLES * 2 * sizeof(int32_t));

  bool is_prebuffering = true; // Only true at start or after hw pause
  const size_t PREBUFFER_THRESHOLD = 8000;
  uint32_t consecutive_empty = 0; // Track consecutive empty reads
  const uint32_t EMPTY_THRESHOLD =
      40; // 40 * 20ms = 800ms of silence = real end of speech

  while (m_is_running) {
    if (!m_hw_valid) {
      is_prebuffering = true; // Reset prebuffering on hw pause
      consecutive_empty = 0;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    if (is_prebuffering) {
      size_t fill_bytes = bm.getUsedBytes(Buffers::SPK_RX_BUF);
      if (fill_bytes < PREBUFFER_THRESHOLD) {
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }
      is_prebuffering = false;
    }

    size_t rx_chunk_bytes = 0;
    void *rx_data_ptr = bm.receive(Buffers::SPK_RX_BUF, &rx_chunk_bytes,
                                   pdMS_TO_TICKS(10), MAX_AUDIO_CHUNK_BYTES);

    if (rx_data_ptr == nullptr || rx_chunk_bytes == 0) {
      // Write silence to keep the codec DMA clock running — never re-enter
      // prebuffering here; that caused the gibberish on long 30s+ responses.
      // A real end-of-stream is detected by turn_complete + sustained empty.
      consecutive_empty++;
      esp_codec_dev_write(device, silence_buffer,
                          SILENCE_SAMPLES * 2 * sizeof(int32_t));
      // Only re-enter prebuffering after sustained silence (not momentary
      // bursts)
      if (consecutive_empty >= EMPTY_THRESHOLD) {
        is_prebuffering = true;
        consecutive_empty = 0;
      }
      continue;
    }
    consecutive_empty = 0; // Reset on every real audio chunk

    size_t num_samples = rx_chunk_bytes / sizeof(int16_t);
    memcpy(dma_safe_buffer, rx_data_ptr, rx_chunk_bytes);
    bm.returnItem(Buffers::SPK_RX_BUF, rx_data_ptr);

    // Expand 16-bit mono → 32-bit stereo (left-justify in 32-bit word)
    for (size_t i = 0; i < num_samples; i++) {
      int32_t sample = ((int32_t)dma_safe_buffer[i]) << 16;
      expanded_buffer[2 * i + 0] = sample; // L
      expanded_buffer[2 * i + 1] = sample; // R
    }

    esp_codec_dev_write(device, expanded_buffer,
                        num_samples * 2 * sizeof(int32_t));
  }

  LOGI_HAL("SpeakerPlaybackTask exiting.");
  heap_caps_free(silence_buffer);
  heap_caps_free(dma_safe_buffer);
  heap_caps_free(expanded_buffer);
  m_task_handle = nullptr;
  vTaskDelete(NULL);
}
