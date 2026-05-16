#include "SpeakerPlayback.h"
#include "common/AppLogger.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

struct TaskParam {
  GlobalSystemSettings settings;
  esp_codec_dev_handle_t device;
  RingbufHandle_t rx_buffer;
};

void SpeakerPlaybackTask::start(const GlobalSystemSettings &settings,
                                esp_codec_dev_handle_t device,
                                RingbufHandle_t rx_ring_buffer) {
  // Fix 1: Dynamically allocate to allow multiple instances and prevent
  // stack-overwrite corruption
  TaskParam *param = new TaskParam();
  param->settings = settings;
  param->device = device;
  param->rx_buffer = rx_ring_buffer;

  xTaskCreatePinnedToCore(&SpeakerPlaybackTask::worker, "speaker_playback_task",
                          settings.audio_stack_size, param,
                          settings.audio_task_priority, NULL,
                          settings.audio_core_id);
}

void SpeakerPlaybackTask::worker(void *pvParameters) {
  // Unpack and immediately free the wrapper structure
  TaskParam *param = static_cast<TaskParam *>(pvParameters);
  GlobalSystemSettings settings = param->settings;
  esp_codec_dev_handle_t device = param->device;
  RingbufHandle_t rx_buffer = param->rx_buffer;
  delete param; // Safe from parameter overwrites now

  if (device == nullptr) {
    LOGE_HAL("SpeakerPlaybackTask started without a valid codec device!");
    vTaskDelete(NULL);
    return;
  }

  LOGI_HAL("Mono Audio Pipeline Active (%lu Hz). Core: %d",
           settings.sample_rate, (int)xPortGetCoreID());

  // Allocate intermediate processing buffers strictly in Internal SRAM
  // (DMA-safe)
  const size_t SILENCE_SAMPLES = 320;
  int16_t *silence_buffer = (int16_t *)heap_caps_malloc(
      SILENCE_SAMPLES * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  memset(silence_buffer, 0, SILENCE_SAMPLES * sizeof(int16_t));

  // Fix 3: Internal RAM intermediate payload buffer to decouple PSRAM from
  // Codec/DMA engine
  const size_t MAX_AUDIO_CHUNK_SIZE =
      2048; // Adjust to your target network/audio payload frame size
  int16_t *dma_safe_buffer = (int16_t *)heap_caps_malloc(
      MAX_AUDIO_CHUNK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  bool is_prebuffering = true;
  const size_t PREBUFFER_THRESHOLD = 16000; // ~500ms safety buffer

  while (true) {
    size_t rx_chunk_bytes = 0;

    // 1. Jitter Buffer / Pre-buffering
    if (is_prebuffering) {
      vRingbufferGetInfo(rx_buffer, NULL, NULL, NULL, NULL, &rx_chunk_bytes);
      if (rx_chunk_bytes < PREBUFFER_THRESHOLD) {
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }
      is_prebuffering = false;
      // LOGI_HAL("Jitter Buffer Ready. Starting playback.");
    }

    // 2. Atomic Frame Receive using BYTEBUF mode
    // We request up to MAX_AUDIO_CHUNK_SIZE to process regular sequential
    // frames
    void *rx_data_ptr = xRingbufferReceiveUpTo(
        rx_buffer, &rx_chunk_bytes, pdMS_TO_TICKS(10), MAX_AUDIO_CHUNK_SIZE);

    if (rx_data_ptr == nullptr || rx_chunk_bytes == 0) {
      // Inject silence to keep DAC clock continuous and prevent pop/click noise
      esp_codec_dev_write(device, silence_buffer,
                          SILENCE_SAMPLES * sizeof(int16_t));
      is_prebuffering = true;
      continue;
    }

    // Fix 3: Copy data out of PSRAM cache space into deterministic Internal
    // DRAM
    memcpy(dma_safe_buffer, rx_data_ptr, rx_chunk_bytes);

    // Release the RingBuffer space immediately after copying to unblock the
    // receiving task
    vRingbufferReturnItem(rx_buffer, rx_data_ptr);

    // 3. Optimized Mono Write using internal RAM target
    // uint64_t start_time = esp_timer_get_time();

    esp_err_t ret =
        esp_codec_dev_write(device, dma_safe_buffer, rx_chunk_bytes);

    // uint64_t end_time = esp_timer_get_time();
    // uint64_t duration = end_time - start_time;

    if (ret != ESP_OK) {
      LOGE_HAL("Codec write error: %d", ret);
    }

    // if (duration > 10000) { // Log if write takes longer than 10ms
    //   LOGW_HAL("Slow codec write: %llu us for %u bytes", duration,
    //            rx_chunk_bytes);
    // }
  }

  heap_caps_free(silence_buffer);
  heap_caps_free(dma_safe_buffer);
  vTaskDelete(NULL);
}
