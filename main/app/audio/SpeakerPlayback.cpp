#include "SpeakerPlayback.h"
#include "common/AppLogger.h"
#include "esp_timer.h"
#include <cstring>
#include <cstdlib>

// Defines + registers the SPK_RX_BUF ring buffer with BufferManager
DEFINE_BUFFER(SPK_RX_BUF, "spk_rx", 1024 * 1024)



#include "common/thread_config.h"
#include "common/sysdb/EmbeddedSysDb.h"

void SpeakerPlaybackTask::start(esp_codec_dev_handle_t device) {
  this->m_device      = device;
  TaskBase::start();
}

void SpeakerPlaybackTask::stop() {
  if (m_task_handle != nullptr) {
    m_running = false;
    while (m_task_handle != nullptr) {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// run() — Leaky Bucket drain loop
//
// The consumer wakes every DRAIN_PERIOD_MS milliseconds (via vTaskDelayUntil)
// and drains whatever PCM data is available from SPK_RX_BUF.  If the buffer
// is empty, a short silence frame is written to keep the I2S DMA clock alive.
//
// This decouples the consumer cadence from the bursty producer (GeminiProtocol
// WebSocket frames) so that playback is always smooth regardless of network
// timing.  No fill-level gating, no re-buffering stalls.
// ─────────────────────────────────────────────────────────────────────────────
void SpeakerPlaybackTask::run() {
  uint32_t sample_rate = EmbeddedSysDb::getInstance().snapshot().audio.sample_rate;
  esp_codec_dev_handle_t device = m_device;
  if (device == nullptr) {
    LOGE_HAL("SpeakerPlaybackTask started without a valid codec device!");
    m_running = false;
    return;
  }

  LOGI_HAL("Speaker Audio Pipeline Active (%lu Hz) — leaky-bucket mode.", (unsigned long)sample_rate);

  int16_t *dma_safe_buffer = (int16_t *)heap_caps_malloc(
      MAX_AUDIO_CHUNK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  int32_t *expanded_buffer = (int32_t *)heap_caps_malloc(
      EXPANDED_BUF_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  int32_t *silence_buffer = (int32_t *)heap_caps_malloc(
      SILENCE_SAMPLES * 2 * sizeof(int32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  if (!dma_safe_buffer || !expanded_buffer || !silence_buffer) {
    LOGE_HAL("Failed to allocate SpeakerPlayback buffers!");
    if (dma_safe_buffer) heap_caps_free(dma_safe_buffer);
    if (expanded_buffer) heap_caps_free(expanded_buffer);
    if (silence_buffer)  heap_caps_free(silence_buffer);
    m_running = false;
    return;
  }
  memset(silence_buffer, 0, SILENCE_SAMPLES * 2 * sizeof(int32_t));

  auto &bm = BufferManager::getInstance();

  // Leaky bucket state
  TickType_t last_wake           = xTaskGetTickCount();
  uint32_t   sustained_empty     = 0; // consecutive empty ticks

  while (m_running) {
    // ── Fixed-rate wakeup — this is the "drain clock" of the leaky bucket ──
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(DRAIN_PERIOD_MS));

    // ── Hardware pause handshake ─────────────────────────────────────────────
    // When AudioService calls pauseHardware() before a clock switch, we must
    // confirm we are out of esp_codec_dev_write() before the caller proceeds.
    if (!m_hw_valid) {
      m_hw_paused_ack = true;    // Signal: safe to reconfigure I2S clock now
      last_wake = xTaskGetTickCount(); // Re-anchor timer to avoid burst catch-up
      sustained_empty = 0;
      continue;
    }
    m_hw_paused_ack = false;

    // ── Drain one chunk (non-blocking) ───────────────────────────────────────
    // The loop timing is entirely controlled by vTaskDelayUntil above, so we
    // use a zero-timeout receive here.
    size_t rx_bytes = 0;
    void  *rx_ptr   = bm.receive(Buffers::SPK_RX_BUF, &rx_bytes, 0, MAX_AUDIO_CHUNK_BYTES);

    if (rx_ptr != nullptr && rx_bytes > 0) {
      // Real audio data arrived — reset empty counter and play it
      sustained_empty = 0;

      size_t num_samples = rx_bytes / sizeof(int16_t);
      memcpy(dma_safe_buffer, rx_ptr, rx_bytes);
      bm.returnItem(Buffers::SPK_RX_BUF, rx_ptr);

      // Expand 16-bit mono → 32-bit stereo (left-justify in 32-bit word)
      for (size_t i = 0; i < num_samples; i++) {
        int32_t sample = ((int32_t)dma_safe_buffer[i]) << 16;
        expanded_buffer[2 * i + 0] = sample; // L
        expanded_buffer[2 * i + 1] = sample; // R
      }

      esp_codec_dev_write(device, expanded_buffer,
                          num_samples * 2 * sizeof(int32_t));

    } else {
      // Buffer empty — write a short silence frame to keep the DMA clock alive
      esp_codec_dev_write(device, silence_buffer,
                          SILENCE_SAMPLES * 2 * sizeof(int32_t));
      sustained_empty++;

      // Only finalize turn_complete after SUSTAINED silence to avoid
      // cutting off the last PCM frames that arrive just before the
      // turnComplete JSON message over the WebSocket.
      if (sustained_empty >= TURN_COMPLETE_DRAIN_TICKS) {
        if (EmbeddedSysDb::getInstance().turnCompletePending()) {
          LOGI_HAL("SpeakerPlayback: sustained empty after turn_complete — finalising.");
          EmbeddedSysDb::getInstance().mutate([](SystemState &s) {
            s.audio.turn_complete_pending = false;
            s.audio.assistant_speaking    = false;
          });
          sustained_empty = 0;
        }
      }
    }
  }

  LOGI_HAL("SpeakerPlaybackTask exiting.");
  heap_caps_free(silence_buffer);
  heap_caps_free(dma_safe_buffer);
  heap_caps_free(expanded_buffer);
}
