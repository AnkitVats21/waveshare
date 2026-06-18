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
   * @brief Pause/resume physical speaker playback calls during clock switches.
   *        pauseHardware() is non-blocking; callers should poll isHardwarePaused()
   *        before touching the I2S clock to avoid a DMA-mid-transfer race.
   */
  void pauseHardware()  { m_hw_valid = false; }
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
  // ── Leaky bucket timing ──────────────────────────────────────────────────
  // The consumer drains at a fixed cadence regardless of how bursty the
  // producer (GeminiProtocol) is.
  // 10 ms = 1 tick at the ESP-IDF default 100 Hz FreeRTOS tick rate.
  // pdMS_TO_TICKS(5) truncates to 0, which causes xTaskDelayUntil to assert.
  static constexpr uint32_t DRAIN_PERIOD_MS          = 10;

  // Silence written each empty tick to keep the I2S DMA clock alive.
  // 240 samples ≈ 10 ms at 24 kHz mono.
  static constexpr size_t   SILENCE_SAMPLES           = 240;

  // Number of consecutive empty ticks required before acting on turn_complete.
  // Prevents premature cutoff when the last PCM frame arrives just before
  // the turnComplete JSON message.  4 × 5 ms = 20 ms of sustained silence.
  static constexpr uint32_t TURN_COMPLETE_DRAIN_TICKS = 4;

  // ── I/O chunk sizing ─────────────────────────────────────────────────────
  static constexpr size_t   MAX_AUDIO_CHUNK_SAMPLES   = 1024;
  static constexpr size_t   MAX_AUDIO_CHUNK_BYTES     = MAX_AUDIO_CHUNK_SAMPLES * sizeof(int16_t);
  static constexpr size_t   EXPANDED_BUF_BYTES        = MAX_AUDIO_CHUNK_SAMPLES * 2 * sizeof(int32_t);

  // ── State ─────────────────────────────────────────────────────────────────
  volatile bool             m_hw_valid      = true;
  volatile bool             m_hw_paused_ack = false; ///< Set when task exits codec_dev_write
  esp_codec_dev_handle_t    m_device        = nullptr;
};
