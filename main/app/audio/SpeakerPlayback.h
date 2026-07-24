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
// Size: 512 KB in PSRAM (holds ~1 s of 16-bit mono at 16 kHz).
// ---------------------------------------------------------------------------
// 32 KB ≈ 12 × 20 ms drain frames @ 32 kHz mono.  Sized close to the I2S DMA
// footprint.  GeminiPCMDrainerTask and AudioEngine both apply back-pressure via
// timed bm.send() calls, so the old 512 KB "absorb the burst" buffer is no longer
// required.
DECLARE_BUFFER(SPK_RX_BUF, "spk_rx", 32 * 1024)

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

  /**
   * @brief Pause/resume the audio draining from SPK_RX_BUF.
   *        Unlike pauseHardware(), this does not release the codec device
   *        or stop the clock, it just plays silence and preserves SPK_RX_BUF data.
   */
  void setPaused(bool paused) { m_paused = paused; }
  bool isPaused() const { return m_paused; }

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

  // ── Amplifier auto-shutoff ───────────────────────────────────────────────
  // After AMP_IDLE_SHUTOFF_MS of continuous silence the PA_EN rail is cut to
  // save battery.  When audio arrives again the rail is re-enabled and we
  // wait AMP_WARMUP_MS for the DAC bias to stabilise before the first write.
  // While the amp is off the poll period is stretched to AMP_IDLE_POLL_MS to
  // further reduce CPU/scheduler overhead.
  static constexpr uint32_t AMP_IDLE_SHUTOFF_MS       = 3000; // 3 s silence → PA off
  static constexpr uint32_t AMP_IDLE_POLL_MS          = 100;  // poll rate when PA is off
  static constexpr uint32_t AMP_WARMUP_MS             = 10;   // PA-on → first write delay

  // ── I/O chunk sizing ─────────────────────────────────────────────────────
  static constexpr size_t   MAX_AUDIO_CHUNK_SAMPLES   = 2048;
  static constexpr size_t   MAX_AUDIO_CHUNK_BYTES     = MAX_AUDIO_CHUNK_SAMPLES * sizeof(int16_t);
  static constexpr size_t   EXPANDED_BUF_BYTES        = MAX_AUDIO_CHUNK_SAMPLES * 2 * sizeof(int32_t);
  static constexpr size_t   MAX_SILENCE_SAMPLES       = 512;

  // ── State ─────────────────────────────────────────────────────────────────
  volatile bool             m_hw_valid           = true;
  volatile bool             m_hw_paused_ack      = false; ///< Set when task exits codec_dev_write
  esp_codec_dev_handle_t    m_device             = nullptr;
  SemaphoreHandle_t         m_pause_sem          = nullptr;

  uint32_t                  m_sustained_empty_ms = 0;    ///< Consecutive ms of empty buffer
  bool                      m_amp_enabled        = true; ///< Whether PA_EN rail is currently on
  volatile bool             m_paused             = false;
};
