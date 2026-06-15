#pragma once

#include "common/app_types.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "services/BufferManager.h"

// ---------------------------------------------------------------------------
// Buffer declaration — this subsystem owns the network→speaker ring buffer.
// Size: 64 KB in PSRAM (holds ~1 s of 16-bit mono at 16 kHz).
// ---------------------------------------------------------------------------
DECLARE_BUFFER(SPK_RX_BUF, "spk_rx", 64 * 1024)

#include "common/TaskBase.h"
#include "common/thread_config.h"

class SpeakerPlaybackTask : public TaskBase {
public:
  SpeakerPlaybackTask()
      : TaskBase({
            "speaker_playback_task",
            8 * 1024,
            ThreadConfig::Priority::SPEAKER_PLAYBACK,
            ThreadConfig::CORE_AUDIO
        }) {}

  /**
   * @brief Start the speaker playback task
   * @param device Pre-initialized codec device handle
   */
  void start(esp_codec_dev_handle_t device);

  /**
   * @brief Cleanly stop the task
   */
  void stop() override;

  /**
   * @brief Pause/resume physical speaker playback calls during clock switches
   */
  void pauseHardware()  { m_hw_valid = false; }
  void resumeHardware() { m_hw_valid = true;  }

protected:
  /**
   * @brief Internal worker thread for playing audio
   */
  void run() override;

private:
  bool processPlayback(int16_t* dma_safe_buffer, int32_t* expanded_buffer, int32_t* silence_buffer);

  static constexpr size_t PREBUFFER_THRESHOLD = 24000;
  static constexpr uint32_t EMPTY_THRESHOLD = 40;
  static constexpr size_t SILENCE_SAMPLES = 320;
  static constexpr size_t MAX_AUDIO_CHUNK_SAMPLES = 1024;
  static constexpr size_t MAX_AUDIO_CHUNK_BYTES = MAX_AUDIO_CHUNK_SAMPLES * sizeof(int16_t);
  static constexpr size_t EXPANDED_BUF_BYTES = MAX_AUDIO_CHUNK_SAMPLES * 2 * sizeof(int32_t);

  volatile bool m_hw_valid = true;
  esp_codec_dev_handle_t m_device = nullptr;
  bool m_is_prebuffering = true;
  uint32_t m_consecutive_empty = 0;
};
