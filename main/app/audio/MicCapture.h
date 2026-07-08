#pragma once

#include "common/app_types.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "services/BufferManager.h"

// ---------------------------------------------------------------------------
// Buffer declaration — this subsystem owns the microphone→network ring buffer.
// Size: 128 KB in PSRAM (holds ~2.7 s of 16-bit mono at 24 kHz).
// ---------------------------------------------------------------------------
DECLARE_BUFFER(MIC_TX_BUF, "mic_tx", 128 * 1024)
DECLARE_BUFFER(RTP_MIC_BUF, "rtp_mic", 64 * 1024)

#include "common/TaskBase.h"
#include "common/thread_config.h"

class AudioHal;

/**
 * @brief Task for capturing audio from the microphone and sending it to a ring
 * buffer
 */
class MicCaptureTask : public TaskBase {
public:
  explicit MicCaptureTask(AudioHal& hal)
      : TaskBase({
            "mic_capture_task",
            8 * 1024,
            ThreadConfig::Priority::MIC_CAPTURE,
            ThreadConfig::CORE_AUDIO
        })
      , m_hal(hal), m_handle(nullptr) {}

  /**
   * @brief Start the microphone capture task
   * @param handle Pre-initialized I2S handle
   */
  void start(i2s_chan_handle_t handle);

  /**
   * @brief Cleanly stops the microphone capture task loop and frees memory
   */
  void stop() override;

  /**
   * @brief Soft enable/disable of the capture loop
   */
  void setEnabled(bool enabled) { m_is_enabled = enabled; }

protected:
  /**
   * @brief Actual object-oriented worker thread execution loop
   */
  void run() override;

private:
  bool processCapture(int16_t* raw_buffer, int16_t* pcm_buffer, size_t raw_bytes, size_t chunk_bytes, int feed_ch, size_t samples_per_chunk);

  AudioHal&         m_hal;
  i2s_chan_handle_t m_handle;
  volatile bool m_is_enabled = true;
  float *m_lms_coeffs;
  float *m_lms_delay_line;
  static constexpr int LMS_FILTER_SIZE = 128; // Adjust for echo tail length
  static constexpr float LMS_LEARNING_RATE = 0.05f;
};
