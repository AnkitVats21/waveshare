#pragma once

#include "common/app_types.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h" // Added for TaskHandle_t
#include "freertos/ringbuf.h"
#include "freertos/task.h"

/**
 * @brief Task for capturing audio from the microphone and sending it to a ring
 * buffer
 */
class MicCaptureTask {
public:
  MicCaptureTask()
      : m_handle(nullptr), m_tx_buffer(nullptr), m_task_handle(nullptr),
        m_is_running(false) {}

  /**
   * @brief Start the microphone capture task
   * @param settings System settings
   * @param handle Pre-initialized I2S handle
   * @param tx_ring_buffer Buffer to send captured audio to
   */
  void start(const GlobalSystemSettings &settings, i2s_chan_handle_t handle,
             RingbufHandle_t tx_ring_buffer);

  /**
   * @brief Cleanly stops the microphone capture task loop and frees memory
   */
  void stop();

private:
  // Persistent state variables inside the class instance context
  GlobalSystemSettings m_settings;
  i2s_chan_handle_t m_handle;
  RingbufHandle_t m_tx_buffer;
  TaskHandle_t m_task_handle;
  volatile bool m_is_running;
  float *m_lms_coeffs;
  float *m_lms_delay_line;
  static constexpr int LMS_FILTER_SIZE = 128; // Adjust for echo tail length
  static constexpr float LMS_LEARNING_RATE = 0.05f;

  /**
   * @brief Static bridge required by FreeRTOS to jump into the C++ instance
   * context
   */
  static void worker_bridge(void *pvParameters);

  /**
   * @brief Actual object-oriented worker thread execution loop
   */
  void worker();
};
