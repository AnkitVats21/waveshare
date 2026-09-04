#pragma once

#include "common/app_types.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "services/BufferManager.h"

// ---------------------------------------------------------------------------
// Buffer declarations — dedicated ring buffers for Multi-Track Audio Mixing:
//   - VOICE_RX_BUF: Gemini Live voice stream (256 KB PSRAM)
//   - ALERT_RX_BUF: Chimes, system notifications, tones (64 KB PSRAM)
//   - MEDIA_RX_BUF: Music playback, local files (512 KB PSRAM)
// ---------------------------------------------------------------------------
DECLARE_BUFFER(VOICE_RX_BUF, "spk_voice", 256 * 1024)
DECLARE_BUFFER(ALERT_RX_BUF, "spk_alert", 64 * 1024)
DECLARE_BUFFER(MEDIA_RX_BUF, "spk_media", 512 * 1024)

namespace Buffers {
    // Backward compatibility alias for legacy references
    static constexpr auto& SPK_RX_BUF = MEDIA_RX_BUF;
}

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
   */
  void pauseHardware()  {
      m_hw_valid = false;
      if (m_pause_sem) {
          xSemaphoreTake(m_pause_sem, pdMS_TO_TICKS(100)); // wait up to 100ms for ack
      }
  }
  void resumeHardware() { m_hw_paused_ack = false; m_hw_valid = true; }

  bool isHardwarePaused() const { return m_hw_paused_ack; }

  /**
   * @brief Set target software ducking gain on the Media track with smooth slew-rate ramping.
   * @param targetGain Gain between 0.0f (muted) and 1.0f (full volume)
   * @param rampMs Duration in milliseconds to ramp to target gain (prevents clicks)
   */
  void setMediaGain(float targetGain, uint32_t rampMs = 50) {
      if (targetGain < 0.0f) targetGain = 0.0f;
      if (targetGain > 1.0f) targetGain = 1.0f;
      m_target_media_gain = targetGain;
      if (rampMs == 0) {
          m_media_gain = targetGain;
          m_media_ramp_step = 0.0f;
      } else {
          uint32_t ramp_samples = (32000 * rampMs) / 1000;
          if (ramp_samples == 0) ramp_samples = 1;
          m_media_ramp_step = (m_target_media_gain - m_media_gain) / (float)ramp_samples;
      }
  }

  float getMediaGain() const { return m_media_gain; }

protected:
  /**
   * @brief Internal worker thread — multi-track audio mixer & drain loop.
   */
  void run() override;

private:
  // ── Playback timing ──────────────────────────────────────────────────────
  static constexpr uint32_t TARGET_FRAME_MS           = 20;
  static constexpr uint32_t EMPTY_FILL_MS             = 10;
  static constexpr uint32_t TURN_COMPLETE_DRAIN_TICKS = 15;
  static constexpr size_t   MIN_JITTER_CUSHION_BYTES  = 4800;

  // ── I/O chunk sizing ─────────────────────────────────────────────────────
  static constexpr size_t   MAX_AUDIO_CHUNK_SAMPLES   = 2048;
  static constexpr size_t   MAX_AUDIO_CHUNK_BYTES     = MAX_AUDIO_CHUNK_SAMPLES * sizeof(int16_t);
  static constexpr size_t   EXPANDED_BUF_BYTES        = MAX_AUDIO_CHUNK_SAMPLES * 2 * sizeof(int32_t);
  static constexpr size_t   MAX_SILENCE_SAMPLES       = 512;

  // ── State ─────────────────────────────────────────────────────────────────
  volatile bool             m_hw_valid          = true;
  volatile bool             m_hw_paused_ack     = false;
  bool                      m_buffering         = true;
  esp_codec_dev_handle_t    m_device            = nullptr;
  SemaphoreHandle_t         m_pause_sem         = nullptr;

  // ── Mixer Gains ───────────────────────────────────────────────────────────
  volatile float            m_media_gain        = 1.0f;
  volatile float            m_target_media_gain = 1.0f;
  volatile float            m_media_ramp_step   = 0.0f;
};

