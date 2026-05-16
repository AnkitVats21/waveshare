#pragma once

#include "common/app_types.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

/**
 * @brief Task for taking audio from a ring buffer and playing it through the
 * speaker
 */
class SpeakerPlaybackTask {
public:
  /**
   * @brief Start the speaker playback task
   * @param settings System settings
   * @param device Pre-initialized codec device handle
   * @param rx_ring_buffer Buffer to read playback audio from
   */
  static void start(const GlobalSystemSettings &settings,
                    esp_codec_dev_handle_t device,
                    RingbufHandle_t rx_ring_buffer);

private:
  struct TaskParam {
    GlobalSystemSettings settings;
    esp_codec_dev_handle_t device;
    RingbufHandle_t rx_buffer;
  };

  /**
   * @brief Internal worker thread for playing audio
   */
  static void worker(void *pvParameters);
};
