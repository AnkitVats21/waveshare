#include "OpusPlayer.h"
#include "app/audio/SpeakerPlayback.h"
#include "common/thread_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace micro_opus;

// Define and register the shared compressed-data ring buffer in PSRAM
DEFINE_BUFFER_WITH_TYPE(OPUS_COMP_BUF, "opus_comp", 512 * 1024,
                        RINGBUF_TYPE_NOSPLIT)

OpusPlayer &OpusPlayer::getInstance() {
  static OpusPlayer instance;
  return instance;
}

OpusPlayer::OpusPlayer()
    : TaskBase({
          "opus_player_task",
          ThreadConfig::STACK_NORMAL,     // stack size
          ThreadConfig::Priority::NORMAL, // Priority (5)
          ThreadConfig::CORE_NETWORK // Core 0 (WiFi, network, decoder tasks)
      }) {
  // Pre-allocate PCM buffer size to 32 KB (max Opus frame is 120ms at 48kHz
  // stereo = 23,040 bytes)
  m_pcm_buffer_size = 32768;
  m_pcm_buffer = (int16_t *)heap_caps_malloc(
      m_pcm_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (m_pcm_buffer == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate initial PCM buffer in PSRAM!");
  }

  // Pre-allocate resample buffer (4096 samples is enough for 120ms at 32kHz =
  // 3840 samples)
  m_resample_buffer_samples = 4096;
  m_resample_buffer =
      (int16_t *)heap_caps_malloc(m_resample_buffer_samples * sizeof(int16_t),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (m_resample_buffer == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate initial resample buffer in PSRAM!");
  }
}

OpusPlayer::~OpusPlayer() {
  stopPlayback();
  if (m_pcm_buffer) {
    heap_caps_free(m_pcm_buffer);
    m_pcm_buffer = nullptr;
  }
  if (m_resample_buffer) {
    heap_caps_free(m_resample_buffer);
    m_resample_buffer = nullptr;
  }
}

bool OpusPlayer::play(AudioSource *source) {
  if (source == nullptr)
    return false;
  ESP_LOGI(TAG, "Playback requested using audio source");

  if (m_playing) {
    ESP_LOGI(TAG, "Stopping active playback first...");
    stopPlayback();
  }

  m_source = source;
  m_stopPlayback = false;

  // Reset decoder and clear buffers before starting the source
  m_decoder.reset();
  BufferManager::getInstance().flush(Buffers::OPUS_COMP_BUF);

  // Reset and start the source producer task
  m_source->resetSource();
  if (!m_source->start()) {
    ESP_LOGE(TAG, "Failed to start the audio source task!");
    m_source = nullptr;
    return false;
  }

  return true;
}

bool OpusPlayer::play(const char *path) {
  ESP_LOGI(TAG, "Legacy play requested for file path: %s", path);
  m_sd_source.setFilePath(path);
  return play(&m_sd_source);
}

void OpusPlayer::stopPlayback() {
  if (m_playing.load()) {
    m_stopPlayback.store(true);

    // Signal the source task to stop
    if (m_source) {
      m_source->stopSource();
    }

    // Send an EOF marker to unblock the consumer task if blocked on receive
    AudioChunkHeader eof_header = {ChunkType::EOF_STREAM, 0};
    BufferManager::getInstance().send(Buffers::OPUS_COMP_BUF, &eof_header,
                                      sizeof(eof_header));

    // Flush the speaker buffer to stop playback immediately
    BufferManager::getInstance().flush(Buffers::SPK_RX_BUF);

    // Wait until consumer loop exits and m_playing is cleared
    while (m_playing.load()) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}

void OpusPlayer::run() {
  ESP_LOGI(TAG, "Consumer task running.");

  while (true) {
    // Block-receive the first packet of a stream
    size_t rx_bytes = 0;
    void *rx_ptr = BufferManager::getInstance().receive(
        Buffers::OPUS_COMP_BUF, &rx_bytes, portMAX_DELAY);
    if (rx_ptr == nullptr) {
      continue;
    }

    AudioChunkHeader *current_chunk =
        reinterpret_cast<AudioChunkHeader *>(rx_ptr);
    size_t current_offset = 0;

    if (current_chunk->type == ChunkType::EOF_STREAM) {
      ESP_LOGI(TAG, "Received EOF marker in stream while idle.");
      BufferManager::getInstance().returnItem(Buffers::OPUS_COMP_BUF, rx_ptr);
      continue;
    } else if (current_chunk->type == ChunkType::ERROR) {
      ESP_LOGE(TAG, "Received ERROR marker in stream while idle.");
      BufferManager::getInstance().returnItem(Buffers::OPUS_COMP_BUF, rx_ptr);
      continue;
    }

    ESP_LOGI(TAG, "Starting decoding pipeline.");
    m_playing.store(true);
    m_stopPlayback.store(false);

    // Consumer decoding loop
    while (m_playing.load() && !m_stopPlayback.load()) {
      // 1. Get the next chunk from the ring buffer if we don't have one
      if (current_chunk == nullptr) {
        size_t rx_bytes = 0;
        void *rx_ptr = BufferManager::getInstance().receive(
            Buffers::OPUS_COMP_BUF, &rx_bytes, portMAX_DELAY);
        if (rx_ptr == nullptr) {
          break;
        }

        current_chunk = reinterpret_cast<AudioChunkHeader *>(rx_ptr);
        current_offset = 0;

        if (current_chunk->type == ChunkType::EOF_STREAM) {
          ESP_LOGI(TAG, "Received EOF marker in stream.");
          BufferManager::getInstance().returnItem(Buffers::OPUS_COMP_BUF,
                                                  rx_ptr);
          current_chunk = nullptr;
          break;
        } else if (current_chunk->type == ChunkType::ERROR) {
          ESP_LOGE(TAG, "Received ERROR marker in stream. Aborting playback.");
          BufferManager::getInstance().returnItem(Buffers::OPUS_COMP_BUF,
                                                  rx_ptr);
          current_chunk = nullptr;
          break;
        }
      }

      uint8_t *payload_data =
          reinterpret_cast<uint8_t *>(current_chunk) + sizeof(AudioChunkHeader);
      size_t payload_len = current_chunk->size;

      size_t bytes_consumed = 0;
      size_t samples_decoded = 0;

      OggOpusResult result = m_decoder.decode(
          payload_data + current_offset, payload_len - current_offset,
          reinterpret_cast<uint8_t *>(m_pcm_buffer), m_pcm_buffer_size,
          bytes_consumed, samples_decoded);

      if (result == OGG_OPUS_OUTPUT_BUFFER_TOO_SMALL) {
        ESP_LOGE(
            TAG,
            "PCM buffer too small (%zu bytes), required %zu. Playback aborted.",
            m_pcm_buffer_size, m_decoder.get_required_output_buffer_size());
        break;
      } else if (result != OGG_OPUS_OK) {
        ESP_LOGE(TAG, "Opus decode error: %d", result);
        break;
      }

      if (bytes_consumed > 0) {
        current_offset += bytes_consumed;
      }

      // If we have fully consumed this chunk, or no progress is made, return
      // the item to release the memory
      if (current_offset >= payload_len || bytes_consumed == 0) {
        BufferManager::getInstance().returnItem(Buffers::OPUS_COMP_BUF,
                                                current_chunk);
        current_chunk = nullptr;
      }

      // Process and output PCM samples
      if (samples_decoded > 0) {
        uint8_t channels = m_decoder.get_channels();
        int16_t *pcm_mono = m_pcm_buffer;
        size_t mono_samples = samples_decoded;

        // Stereo to Mono downmix in-place
        if (channels == 2) {
          for (size_t i = 0; i < samples_decoded; ++i) {
            int32_t mix = (static_cast<int32_t>(m_pcm_buffer[2 * i]) +
                           static_cast<int32_t>(m_pcm_buffer[2 * i + 1])) /
                          2;
            m_pcm_buffer[i] = static_cast<int16_t>(mix);
          }
        }

        // Resample 48 kHz mono to 32 kHz mono (3:2 downsampling)
        uint32_t src_rate = m_decoder.get_sample_rate();
        uint32_t dst_rate = 32000;

        size_t resampled_count =
            static_cast<size_t>(mono_samples * dst_rate / src_rate);
        if (resampled_count > m_resample_buffer_samples) {
          ESP_LOGE(TAG,
                   "Resample buffer too small (%zu samples), required %zu. "
                   "Playback aborted.",
                   m_resample_buffer_samples, resampled_count);
          break;
        }

        float ratio = static_cast<float>(src_rate) / dst_rate;
        for (size_t j = 0; j < resampled_count; ++j) {
          float src_pos = j * ratio;
          size_t idx = static_cast<size_t>(src_pos);
          float frac = src_pos - idx;
          if (idx + 1 < mono_samples) {
            float s0 = pcm_mono[idx];
            float s1 = pcm_mono[idx + 1];
            m_resample_buffer[j] = static_cast<int16_t>(s0 + frac * (s1 - s0));
          } else {
            m_resample_buffer[j] = pcm_mono[idx];
          }
        }

        // Push final 32 kHz mono PCM to SPK_RX_BUF
        size_t send_bytes = resampled_count * sizeof(int16_t);
        BufferManager::getInstance().send(
            Buffers::SPK_RX_BUF, m_resample_buffer, send_bytes,
            pdMS_TO_TICKS(100) // blocks if SPK_RX_BUF is full, naturally
                               // throttling decode speed
        );
      } else {
        vTaskDelay(pdMS_TO_TICKS(1));
      }
    }

    // Cleanup any partially processed chunk
    if (current_chunk != nullptr) {
      BufferManager::getInstance().returnItem(Buffers::OPUS_COMP_BUF,
                                              current_chunk);
      current_chunk = nullptr;
    }

    // Clean up the source task if active
    m_stopPlayback.store(true);
    if (m_source) {
      m_source->stopSource();
      m_source = nullptr;
    }

    // Flush any remaining items in OPUS_COMP_BUF
    BufferManager::getInstance().flush(Buffers::OPUS_COMP_BUF);

    // Wait until speaker buffer drains completely, unless stopped
    while (BufferManager::getInstance().getUsedBytes(Buffers::SPK_RX_BUF) > 0 &&
           !m_stopPlayback.load()) {
      vTaskDelay(pdMS_TO_TICKS(20));
    }

    m_playing.store(false);
    ESP_LOGI(TAG, "Playback completed cleanly.");
  }
}
