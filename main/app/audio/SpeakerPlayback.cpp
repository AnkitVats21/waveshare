#include "SpeakerPlayback.h"
#include "common/AppLogger.h"
#include "esp_timer.h"
#include <cstring>
#include <cstdlib>

// Defines + registers the SPK_RX_BUF ring buffer with BufferManager
DEFINE_BUFFER(SPK_RX_BUF, "spk_rx", 1024 * 1024)



#include "common/thread_config.h"
#include "common/sysdb/EmbeddedSysDb.h"

namespace {

TickType_t ticksForAtLeastOnePeriod(uint32_t duration_ms) {
  TickType_t ticks = pdMS_TO_TICKS(duration_ms);
  return ticks > 0 ? ticks : 1;
}

uint32_t getActivePlaybackSampleRate(const SystemState& snap) {
  if (snap.audio.wav_playing && snap.audio.wav_sample_rate > 0) {
    return snap.audio.wav_sample_rate;
  }
  if (snap.assistant.session_state == AssistantState::AssistantSpeaking ||
      snap.audio.turn_complete_pending) {
    return 24000;
  }
  return 16000;
}

size_t samplesForDurationMs(uint32_t sample_rate, uint32_t duration_ms) {
  uint64_t samples = (static_cast<uint64_t>(sample_rate) * duration_ms + 999) / 1000;
  if (samples == 0) samples = 1;
  return static_cast<size_t>(samples);
}

uint32_t durationMsForSamples(size_t samples, uint32_t sample_rate) {
  if (sample_rate == 0 || samples == 0) return 10;
  uint32_t ms = static_cast<uint32_t>(
      (static_cast<uint64_t>(samples) * 1000 + sample_rate - 1) / sample_rate);
  return ms == 0 ? 10 : ms;
}

} // namespace

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
      MAX_SILENCE_SAMPLES * 2 * sizeof(int32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  if (!dma_safe_buffer || !expanded_buffer || !silence_buffer) {
    LOGE_HAL("Failed to allocate SpeakerPlayback buffers!");
    if (dma_safe_buffer) heap_caps_free(dma_safe_buffer);
    if (expanded_buffer) heap_caps_free(expanded_buffer);
    if (silence_buffer)  heap_caps_free(silence_buffer);
    m_running = false;
    return;
  }
  memset(silence_buffer, 0, MAX_SILENCE_SAMPLES * 2 * sizeof(int32_t));

  auto &bm = BufferManager::getInstance();

  // Leaky bucket state
  TickType_t last_wake           = xTaskGetTickCount();
  TickType_t wake_period_ticks   = ticksForAtLeastOnePeriod(EMPTY_FILL_MS);
  uint32_t   sustained_empty     = 0; // consecutive empty ticks

  while (m_running) {
    // ── Duration-based wakeup — match playback cadence to the audio duration ──
    vTaskDelayUntil(&last_wake, wake_period_ticks);

    // ── Hardware pause handshake ─────────────────────────────────────────────
    // When AudioService calls pauseHardware() before a clock switch, we must
    // confirm we are out of esp_codec_dev_write() before the caller proceeds.
    if (!m_hw_valid) {
      if (!m_hw_paused_ack) {
        m_hw_paused_ack = true;    // Signal: safe to reconfigure I2S clock now
        if (m_pause_sem) {
          xSemaphoreGive(m_pause_sem);
        }
      }
      last_wake = xTaskGetTickCount(); // Re-anchor timer to avoid burst catch-up
      wake_period_ticks = ticksForAtLeastOnePeriod(EMPTY_FILL_MS);
      sustained_empty = 0;
      continue;
    }
    m_hw_paused_ack = false;

    auto snap = EmbeddedSysDb::getInstance().snapshot();
    uint32_t active_sample_rate = getActivePlaybackSampleRate(snap);
    size_t target_samples = samplesForDurationMs(active_sample_rate, TARGET_FRAME_MS);
    if (target_samples > MAX_AUDIO_CHUNK_SAMPLES) {
      target_samples = MAX_AUDIO_CHUNK_SAMPLES;
    }
    size_t target_chunk_bytes = target_samples * sizeof(int16_t);

    // ── Drain one chunk (non-blocking) ───────────────────────────────────────
    size_t rx_bytes = 0;
    void  *rx_ptr   = bm.receive(Buffers::SPK_RX_BUF, &rx_bytes, 0, target_chunk_bytes);

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
      wake_period_ticks = ticksForAtLeastOnePeriod(
          durationMsForSamples(num_samples, active_sample_rate));

    } else {
      // Buffer empty — write a short silence frame to keep the DMA clock alive
      size_t silence_samples = samplesForDurationMs(active_sample_rate, EMPTY_FILL_MS);
      if (silence_samples > MAX_SILENCE_SAMPLES) {
        silence_samples = MAX_SILENCE_SAMPLES;
      }
      esp_codec_dev_write(device, silence_buffer,
                          silence_samples * 2 * sizeof(int32_t));
      wake_period_ticks = ticksForAtLeastOnePeriod(EMPTY_FILL_MS);
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
