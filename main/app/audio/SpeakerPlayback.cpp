#include "SpeakerPlayback.h"
#include "AudioOrchestrator.h"
#include "common/AppLogger.h"
#include "esp_timer.h"
#include <cstring>
#include <cstdlib>

// Defines + registers multi-track ring buffers with BufferManager
DEFINE_BUFFER(VOICE_RX_BUF, "spk_voice", 256 * 1024)
DEFINE_BUFFER(ALERT_RX_BUF, "spk_alert", 64 * 1024)
DEFINE_BUFFER(MEDIA_RX_BUF, "spk_media", 512 * 1024)

#include "common/thread_config.h"
#include "common/sysdb/EmbeddedSysDb.h"

namespace {

TickType_t ticksForAtLeastOnePeriod(uint32_t duration_ms) {
  TickType_t ticks = pdMS_TO_TICKS(duration_ms);
  return ticks > 0 ? ticks : 1;
}

size_t samplesForDurationMs(uint32_t sample_rate, uint32_t duration_ms) {
  uint64_t samples = (static_cast<uint64_t>(sample_rate) * duration_ms + 999) / 1000;
  if (samples == 0) samples = 1;
  return static_cast<size_t>(samples);
}

void upsample_3to4(const int16_t* src, int16_t* dst, int src_len) {
    int dst_len = (src_len * 4) / 3;
    for (int j = 0; j < dst_len; j++) {
        int i_in = (j * 3) / 4;
        int rem = (j * 3) % 4;
        if (rem == 0 || (i_in + 1) >= src_len) {
            dst[j] = src[i_in];
        } else {
            int32_t s0 = src[i_in];
            int32_t s1 = src[i_in + 1];
            dst[j] = (int16_t)(((4 - rem) * s0 + rem * s1) >> 2);
        }
    }
}

} // namespace

void SpeakerPlaybackTask::start(esp_codec_dev_handle_t device) {
  this->m_device = device;
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
// run() — Multi-Track Real-Time Audio Mixer & Output Loop
//
// Mixes Voice (Gemini 24kHz upsampled to 32kHz), Alert (32kHz chimes/tones),
// and Media (32kHz music) with smooth ducking gain interpolation.
// Output format to codec: 32 kHz 32-bit stereo.
// ─────────────────────────────────────────────────────────────────────────────
void SpeakerPlaybackTask::run() {
  esp_codec_dev_handle_t device = m_device;
  if (device == nullptr) {
    LOGE_HAL("SpeakerPlaybackTask started without a valid codec device!");
    m_running = false;
    return;
  }

  LOGI_HAL("Speaker Audio Multi-Track Mixer Active (32000 Hz) — Voice/Alert/Media.");

  int32_t *expanded_buffer = (int32_t *)heap_caps_malloc(
      EXPANDED_BUF_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  int32_t *silence_buffer = (int32_t *)heap_caps_malloc(
      MAX_SILENCE_SAMPLES * 2 * sizeof(int32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  int16_t *voice_pcm = (int16_t *)heap_caps_malloc(
      MAX_AUDIO_CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  int16_t *alert_pcm = (int16_t *)heap_caps_malloc(
      MAX_AUDIO_CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  int16_t *media_pcm = (int16_t *)heap_caps_malloc(
      MAX_AUDIO_CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  if (!expanded_buffer || !silence_buffer || !voice_pcm || !alert_pcm || !media_pcm) {
    LOGE_HAL("Failed to allocate Multi-Track Mixer buffers!");
    if (expanded_buffer) heap_caps_free(expanded_buffer);
    if (silence_buffer)  heap_caps_free(silence_buffer);
    if (voice_pcm)       heap_caps_free(voice_pcm);
    if (alert_pcm)       heap_caps_free(alert_pcm);
    if (media_pcm)       heap_caps_free(media_pcm);
    m_running = false;
    return;
  }
  memset(silence_buffer, 0, MAX_SILENCE_SAMPLES * 2 * sizeof(int32_t));

  auto &bm = BufferManager::getInstance();

  TickType_t last_wake         = xTaskGetTickCount();
  TickType_t wake_period_ticks = ticksForAtLeastOnePeriod(EMPTY_FILL_MS);
  uint32_t   sustained_empty   = 0;

  constexpr uint32_t NATIVE_RATE = 32000;
  const size_t target_samples = samplesForDurationMs(NATIVE_RATE, TARGET_FRAME_MS);

  while (m_running) {
    vTaskDelayUntil(&last_wake, wake_period_ticks);

    auto snap = EmbeddedSysDb::getInstance().snapshot();

    bool has_voice = false;
    size_t num_voice_32k = 0;
    bool has_alert = false;
    size_t num_alert = 0;
    bool has_media = false;
    size_t num_media = 0;

    // 1. Voice Track (Gemini Live 24kHz -> 32kHz)
    if (snap.audio.assistant_speaking || snap.audio.turn_complete_pending) {
      if (m_buffering) {
        size_t buffered = bm.getUsedBytes(Buffers::VOICE_RX_BUF);
        if (buffered >= MIN_JITTER_CUSHION_BYTES || snap.audio.turn_complete_pending) {
          m_buffering = false;
        }
      }

      if (!m_buffering) {
        size_t target_voice_24k_samples = (target_samples * 3) / 4;
        size_t target_voice_bytes = target_voice_24k_samples * sizeof(int16_t);
        size_t rx_bytes = 0;
        void *rx_ptr = bm.receive(Buffers::VOICE_RX_BUF, &rx_bytes, 0, target_voice_bytes);

        if (rx_ptr != nullptr && rx_bytes > 0) {
          size_t samples_24k = rx_bytes / sizeof(int16_t);
          num_voice_32k = (samples_24k * 4) / 3;
          if (num_voice_32k > MAX_AUDIO_CHUNK_SAMPLES) num_voice_32k = MAX_AUDIO_CHUNK_SAMPLES;
          upsample_3to4((const int16_t*)rx_ptr, voice_pcm, samples_24k);
          bm.returnItem(Buffers::VOICE_RX_BUF, rx_ptr);
          has_voice = true;
        } else if (!snap.audio.turn_complete_pending) {
          // Starved mid-speech: rebuffer to avoid playing chopped syllables
          m_buffering = true;
        }
      }
    } else {
      m_buffering = true;
    }

    // 2. Alert Track (32kHz mono chimes/tones)
    {
      size_t target_alert_bytes = target_samples * sizeof(int16_t);
      size_t rx_bytes = 0;
      void *rx_ptr = bm.receive(Buffers::ALERT_RX_BUF, &rx_bytes, 0, target_alert_bytes);
      if (rx_ptr != nullptr && rx_bytes > 0) {
        num_alert = rx_bytes / sizeof(int16_t);
        if (num_alert > MAX_AUDIO_CHUNK_SAMPLES) num_alert = MAX_AUDIO_CHUNK_SAMPLES;
        memcpy(alert_pcm, rx_ptr, num_alert * sizeof(int16_t));
        bm.returnItem(Buffers::ALERT_RX_BUF, rx_ptr);
        has_alert = true;
      }
    }

    // 3. Media Track (32kHz music playback / WAV)
    bool is_media_active = AudioOrchestrator::getInstance().isMediaActive();
    if (is_media_active) {
      size_t target_media_bytes = target_samples * sizeof(int16_t);
      size_t rx_bytes = 0;
      void *rx_ptr = bm.receive(Buffers::MEDIA_RX_BUF, &rx_bytes, 0, target_media_bytes);
      if (rx_ptr != nullptr && rx_bytes > 0) {
        num_media = rx_bytes / sizeof(int16_t);
        if (num_media > MAX_AUDIO_CHUNK_SAMPLES) num_media = MAX_AUDIO_CHUNK_SAMPLES;
        memcpy(media_pcm, rx_ptr, num_media * sizeof(int16_t));
        bm.returnItem(Buffers::MEDIA_RX_BUF, rx_ptr);
        has_media = true;
      }
    }

    // ── Multi-Track Mixing & Saturation Clamping ──────────────────────────────
    if (has_voice || has_alert || has_media) {
      sustained_empty = 0;

      // Determine actual frames to write based on active sources
      size_t frames_to_write = target_samples;
      if (has_voice && !has_media && !has_alert) {
        // Pure voice: write exact number of decoded samples (never zero-pad!)
        frames_to_write = num_voice_32k;
      } else if (!has_voice && has_media && !has_alert) {
        frames_to_write = num_media;
      } else if (!has_voice && !has_media && has_alert) {
        frames_to_write = num_alert;
      } else {
        frames_to_write = std::max(num_voice_32k, std::max(num_alert, num_media));
      }
      if (frames_to_write > MAX_AUDIO_CHUNK_SAMPLES) frames_to_write = MAX_AUDIO_CHUNK_SAMPLES;
      if (frames_to_write == 0) frames_to_write = target_samples;

      for (size_t i = 0; i < frames_to_write; ++i) {
        // Linear slew rate interpolation for ducking gain
        if (m_media_gain != m_target_media_gain) {
          m_media_gain += m_media_ramp_step;
          if ((m_media_ramp_step > 0.0f && m_media_gain >= m_target_media_gain) ||
              (m_media_ramp_step < 0.0f && m_media_gain <= m_target_media_gain)) {
            m_media_gain = m_target_media_gain;
            m_media_ramp_step = 0.0f;
          }
        }

        int32_t mix = 0;
        if (has_voice && i < num_voice_32k) {
          mix += (int32_t)voice_pcm[i];
        }
        if (has_alert && i < num_alert) {
          mix += (int32_t)alert_pcm[i];
        }
        if (has_media && i < num_media) {
          mix += (int32_t)((float)media_pcm[i] * m_media_gain);
        }

        // Saturation clamping to avoid harsh int16 overflow wrap-around
        if (mix > 32767) mix = 32767;
        else if (mix < -32768) mix = -32768;

        // Expand 16-bit mono -> 32-bit stereo DMA frame
        int32_t sample32 = ((int32_t)((int16_t)mix)) << 16;
        expanded_buffer[2 * i + 0] = sample32; // L
        expanded_buffer[2 * i + 1] = sample32; // R
      }

      esp_codec_dev_write(device, expanded_buffer, frames_to_write * 2 * sizeof(int32_t));
      uint32_t duration_ms = static_cast<uint32_t>((static_cast<uint64_t>(frames_to_write) * 1000 + NATIVE_RATE - 1) / NATIVE_RATE);
      wake_period_ticks = ticksForAtLeastOnePeriod(duration_ms);

    } else {
      // All tracks starved/idle — output silence to keep I2S DMA alive
      size_t silence_samples = samplesForDurationMs(NATIVE_RATE, EMPTY_FILL_MS);
      if (silence_samples > MAX_SILENCE_SAMPLES) silence_samples = MAX_SILENCE_SAMPLES;

      esp_codec_dev_write(device, silence_buffer, silence_samples * 2 * sizeof(int32_t));
      wake_period_ticks = ticksForAtLeastOnePeriod(EMPTY_FILL_MS);
      sustained_empty++;

      if (snap.audio.assistant_speaking && sustained_empty >= 2 && !snap.audio.turn_complete_pending) {
        m_buffering = true;
      }

      // Finalize turn_complete once voice buffer has thoroughly drained
      if (sustained_empty >= TURN_COMPLETE_DRAIN_TICKS) {
        if (EmbeddedSysDb::getInstance().turnCompletePending()) {
          LOGI_HAL("SpeakerPlayback: sustained empty after turn_complete — finalising.");
          EmbeddedSysDb::getInstance().mutate([](SystemState &s) {
            s.audio.turn_complete_pending = false;
            s.audio.assistant_speaking    = false;
          });
          AudioOrchestrator::getInstance().notifyVoiceEnded();
          sustained_empty = 0;
          m_buffering = true;
        }
      }
    }
  }

  LOGI_HAL("SpeakerPlaybackTask exiting.");
  heap_caps_free(silence_buffer);
  heap_caps_free(expanded_buffer);
  heap_caps_free(voice_pcm);
  heap_caps_free(alert_pcm);
  heap_caps_free(media_pcm);
}
