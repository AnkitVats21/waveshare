#pragma once

#include "common/app_types.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

/**
 * @brief Task for taking audio from a ring buffer and playing it through the
 * speaker
 */
class SpeakerPlaybackTask {
public:
  SpeakerPlaybackTask() : m_task_handle(nullptr), m_is_running(false) {}

  /**
   * @brief Start the speaker playback task
   * @param settings System settings
   * @param device Pre-initialized codec device handle
   * @param rx_ring_buffer Buffer to read playback audio from
   */
  void start(const GlobalSystemSettings &settings,
                    esp_codec_dev_handle_t device,
                    RingbufHandle_t rx_ring_buffer);

  /**
   * @brief Cleanly stop the task
   */
  void stop();

private:
  TaskHandle_t m_task_handle;
  volatile bool m_is_running;

  struct TaskParam {
    SpeakerPlaybackTask* self;
    GlobalSystemSettings settings;
    esp_codec_dev_handle_t device;
    RingbufHandle_t rx_buffer;
  };

  /**
   * @brief Internal worker thread for playing audio
   */
  static void worker_bridge(void *pvParameters);
  void worker(GlobalSystemSettings settings, esp_codec_dev_handle_t device, RingbufHandle_t rx_buffer);
};
