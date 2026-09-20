#include "audio_core/SpeakerPlayback.h"
#include "audio_core/AudioOrchestrator.h"
#include "core_sysdb/AppLogger.h"
#include "audio_core/Resampler.h"
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

size_t samplesForDurationMs(uint32_t sample_rate, uint32_t duration_ms) {
  uint64_t samples = (static_cast<uint64_t>(sample_rate) * duration_ms + 999) / 1000;
  if (samples == 0) samples = 1;
  return static_cast<size_t>(samples);
}

} // namespace

void SpeakerPlaybackTask::start(esp_codec_dev_handle_t device, IAudioSink* customSink) {
  this->m_device = device;
  this->m_custom_sink = customSink;
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
// Mixes Voice (Gemini 24kHz upsampled to 44.1kHz), Alert (44.1kHz chimes/tones),
// and Media (44.1kHz music) with smooth ducking gain interpolation, all in a
// 44.1kHz mixer domain. The mixed track is then downsampled to the local
// hardware rate (32kHz) right before each esp_codec_dev_write().
// ─────────────────────────────────────────────────────────────────────────────
void SpeakerPlaybackTask::run() {
  esp_codec_dev_handle_t device = m_device;
  IAudioSink* custom_sink = m_custom_sink;
  if (device == nullptr && custom_sink == nullptr) {
    LOGE_HAL("SpeakerPlaybackTask started without a valid codec device or custom sink!");
    m_running = false;
    return;
  }

  auto writeAudio = [&](const void* buf, size_t bytes) -> int {
    if (custom_sink != nullptr) {
      size_t written = 0;
      return (custom_sink->write(buf, bytes, &written, 10) == ESP_OK) ? ESP_CODEC_DEV_OK : -1;
    } else if (device != nullptr) {
      return esp_codec_dev_write(device, const_cast<void*>(buf), bytes);
    }
    return ESP_CODEC_DEV_OK;
  };

  LOGI_HAL("Speaker Audio Multi-Track Mixer Active (mixer=%u Hz, onboard=%u Hz) — Voice/Alert/Media.",
           (unsigned)MIXER_SAMPLE_RATE, (unsigned)LOCAL_SAMPLE_RATE);

  LinearResampler resampler;

  int32_t *expanded_buffer = (int32_t *)heap_caps_malloc(
      EXPANDED_BUF_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  int32_t *silence_buffer = (int32_t *)heap_caps_malloc(
      MAX_SILENCE_SAMPLES * 2 * sizeof(int32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  int16_t *voice_pcm = (int16_t *)heap_caps_malloc(
      MAX_AUDIO_CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  int16_t *alert_pcm = (int16_t *)heap_caps_malloc(
      MAX_AUDIO_CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  int16_t *media_pcm = (int16_t *)heap_caps_malloc(
      MAX_AUDIO_CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  int16_t *local_mono_44k = (int16_t *)heap_caps_malloc(
      MAX_AUDIO_CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  int16_t *local_mono_32k = (int16_t *)heap_caps_malloc(
      MAX_AUDIO_CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  if (!expanded_buffer || !silence_buffer || !voice_pcm || !alert_pcm || !media_pcm ||
      !local_mono_44k || !local_mono_32k) {
    LOGE_HAL("Failed to allocate Multi-Track Mixer buffers!");
    if (expanded_buffer)   heap_caps_free(expanded_buffer);
    if (silence_buffer)    heap_caps_free(silence_buffer);
    if (voice_pcm)         heap_caps_free(voice_pcm);
    if (alert_pcm)         heap_caps_free(alert_pcm);
    if (media_pcm)         heap_caps_free(media_pcm);
    if (local_mono_44k)    heap_caps_free(local_mono_44k);
    if (local_mono_32k)    heap_caps_free(local_mono_32k);
    m_running = false;
    return;
  }
  memset(silence_buffer, 0, MAX_SILENCE_SAMPLES * 2 * sizeof(int32_t));

  auto &bm = BufferManager::getInstance();

  uint32_t sustained_empty = 0;

  constexpr uint32_t NATIVE_RATE = MIXER_SAMPLE_RATE;
  constexpr uint32_t LOCAL_RATE = LOCAL_SAMPLE_RATE;
  constexpr uint32_t VOICE_SRC_RATE = 24000;
  const size_t target_samples = samplesForDurationMs(NATIVE_RATE, TARGET_FRAME_MS);

  while (m_running) {

    auto& sysdb = EmbeddedSysDb::getInstance();
    const uint32_t audio_flags = sysdb.hotAudioFlags();
    const bool asst_speaking   = (audio_flags & HotAudioBit::ASST_SPEAKING) != 0;
    const bool turn_pending    = (audio_flags & HotAudioBit::TURN_COMPLETE_PEND) != 0;

    bool has_voice = false;
    size_t num_voice = 0;
    bool has_alert = false;
    size_t num_alert = 0;
    bool has_media = false;
    size_t num_media = 0;

    // 1. Voice Track (Gemini Live 24kHz -> 44.1kHz)
    if (asst_speaking || turn_pending) {
      if (m_buffering) {
        size_t buffered = bm.getUsedBytes(Buffers::VOICE_RX_BUF);
        if (buffered >= MIN_JITTER_CUSHION_BYTES || turn_pending) {
          m_buffering = false;
        }
      }

      if (!m_buffering) {
        size_t target_voice_24k_samples = samplesForDurationMs(VOICE_SRC_RATE, TARGET_FRAME_MS);
        size_t target_voice_bytes = target_voice_24k_samples * sizeof(int16_t);
        size_t rx_bytes = 0;
        void *rx_ptr = bm.receive(Buffers::VOICE_RX_BUF, &rx_bytes, 0, target_voice_bytes);

        if (rx_ptr != nullptr && rx_bytes > 0) {
          size_t samples_24k = rx_bytes / sizeof(int16_t);
          num_voice = (samples_24k * NATIVE_RATE) / VOICE_SRC_RATE;
          if (num_voice > MAX_AUDIO_CHUNK_SAMPLES) num_voice = MAX_AUDIO_CHUNK_SAMPLES;
          resampler.resample((const int16_t*)rx_ptr, samples_24k, voice_pcm, num_voice, 1);
          bm.returnItem(Buffers::VOICE_RX_BUF, rx_ptr);
          has_voice = true;
        } else if (!turn_pending) {
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
        frames_to_write = num_voice;
      } else if (!has_voice && has_media && !has_alert) {
        frames_to_write = num_media;
      } else if (!has_voice && !has_media && has_alert) {
        frames_to_write = num_alert;
      } else {
        frames_to_write = std::max(num_voice, std::max(num_alert, num_media));
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
        if (has_voice && i < num_voice) {
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

        int16_t s16 = static_cast<int16_t>(mix);
        local_mono_44k[i] = s16;
      }

      // Downsample the mixed mono track to the local hardware rate, then
      // expand to 32-bit stereo DMA frames for the onboard codec.
      size_t local_frames = computeResampledFrames(frames_to_write, NATIVE_RATE, LOCAL_RATE);
      if (local_frames == 0) local_frames = 1;
      if (local_frames > MAX_AUDIO_CHUNK_SAMPLES) local_frames = MAX_AUDIO_CHUNK_SAMPLES;
      resampler.resample(local_mono_44k, frames_to_write, local_mono_32k, local_frames, 1);
      for (size_t i = 0; i < local_frames; ++i) {
        int32_t sample32 = ((int32_t)local_mono_32k[i]) << 16;
        expanded_buffer[2 * i + 0] = sample32; // L
        expanded_buffer[2 * i + 1] = sample32; // R
      }

      int ret = writeAudio(expanded_buffer, local_frames * 2 * sizeof(int32_t));
      if (ret != ESP_CODEC_DEV_OK) {
        vTaskDelay(pdMS_TO_TICKS(10));
      }

    } else {
      // All tracks starved/idle — output silence to keep I2S DMA alive.
      size_t local_silence_samples = samplesForDurationMs(LOCAL_RATE, EMPTY_FILL_MS);
      if (local_silence_samples > MAX_SILENCE_SAMPLES) local_silence_samples = MAX_SILENCE_SAMPLES;

      int ret = writeAudio(silence_buffer, local_silence_samples * 2 * sizeof(int32_t));
      if (ret != ESP_CODEC_DEV_OK) {
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      sustained_empty++;

      if (asst_speaking && sustained_empty >= 2 && !turn_pending) {
        m_buffering = true;
      }

      // Finalize turn_complete once voice buffer has thoroughly drained
      if (sustained_empty >= TURN_COMPLETE_DRAIN_TICKS) {
        if (turn_pending) {
          LOGI_HAL("SpeakerPlayback: sustained empty after turn_complete — finalising.");
          sysdb.mutate([](SystemState &s) {
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
  heap_caps_free(local_mono_44k);
  heap_caps_free(local_mono_32k);
}
