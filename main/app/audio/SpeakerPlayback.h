#pragma once

#include "common/app_types.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "services/BufferManager.h"

// ---------------------------------------------------------------------------
// Buffer declaration — this subsystem owns the network→speaker ring buffer.
// Size: 64 KB in PSRAM (holds ~1 s of 16-bit mono at 16 kHz).
// ---------------------------------------------------------------------------
DECLARE_BUFFER(SPK_RX_BUF, "spk_rx", 1024 * 1024)

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
        }) {
      m_pause_sem = xSemaphoreCreateBinary();
  }

  ~SpeakerPlaybackTask() override {
      if (m_pause_sem) {
          vSemaphoreDelete(m_pause_sem);
      }
  }

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
   * @brief Pause/resume physical speaker playback calls during clock switches.
   *        pauseHardware() is synchronous and blocks until the player thread has acknowledged
   *        the pause and exited the codec write block, preventing I2S DMA race conditions.
   */
  void pauseHardware()  {
      m_hw_valid = false;
      if (m_pause_sem) {
          xSemaphoreTake(m_pause_sem, pdMS_TO_TICKS(100)); // wait up to 100ms for ack
      }
  }
  void resumeHardware() { m_hw_paused_ack = false; m_hw_valid = true; }

  /**
   * @brief Returns true once the playback task has confirmed it is out of
   *        esp_codec_dev_write() and will not touch the codec until resumed.
   *        Callers MUST wait for this before reconfiguring the I2S clock.
   */
  bool isHardwarePaused() const { return m_hw_paused_ack; }

protected:
  /**
   * @brief Internal worker thread — leaky-bucket drain loop.
   */
  void run() override;

private:
  // ── Playback timing ──────────────────────────────────────────────────────
  // Target steady-state frame size is 20 ms. The empty-fill cadence stays at
  // 10 ms to keep the clock alive without injecting large silence bursts.
  static constexpr uint32_t TARGET_FRAME_MS           = 20;
  static constexpr uint32_t EMPTY_FILL_MS             = 10;

  // Number of consecutive empty ticks required before acting on turn_complete.
  static constexpr uint32_t TURN_COMPLETE_DRAIN_TICKS = 2;

  // ── I/O chunk sizing ─────────────────────────────────────────────────────
  static constexpr size_t   MAX_AUDIO_CHUNK_SAMPLES   = 2048;
  static constexpr size_t   MAX_AUDIO_CHUNK_BYTES     = MAX_AUDIO_CHUNK_SAMPLES * sizeof(int16_t);
  static constexpr size_t   EXPANDED_BUF_BYTES        = MAX_AUDIO_CHUNK_SAMPLES * 2 * sizeof(int32_t);
  static constexpr size_t   MAX_SILENCE_SAMPLES       = 512;

  // ── State ─────────────────────────────────────────────────────────────────
  volatile bool             m_hw_valid      = true;
  volatile bool             m_hw_paused_ack = false; ///< Set when task exits codec_dev_write
  esp_codec_dev_handle_t    m_device        = nullptr;
  SemaphoreHandle_t         m_pause_sem     = nullptr;
};
