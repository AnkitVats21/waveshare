#include "app/audio/WavPlayer.h"
#include "services/BufferManager.h"
#include "app/audio/SpeakerPlayback.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "esp_log.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

WavPlayer& WavPlayer::getInstance() {
    static WavPlayer instance;
    return instance;
}

WavPlayer::WavPlayer() {}

WavPlayer::~WavPlayer() {
    stop();
}

bool WavPlayer::playAsync(const char* filepath) {
    ESP_LOGI(TAG, "playAsync requested for file: %s", filepath);

    if (m_playing) {
        ESP_LOGI(TAG, "Stopping active playback first.");
        stop();
    }

    // 1. Open the file synchronously
    FILE* f = fopen(filepath, "rb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "Failed to open WAV file: %s", filepath);
        return false;
    }

    // 2. Parse RIFF header (12 bytes)
    struct RiffHeader {
        char riff[4];
        uint32_t riff_size;
        char wave[4];
    } __attribute__((packed)) riff_header;

    if (fread(&riff_header, 1, 12, f) != 12) {
        ESP_LOGE(TAG, "Failed to read WAV RIFF header");
        fclose(f);
        return false;
    }

    if (strncmp(riff_header.riff, "RIFF", 4) != 0 || strncmp(riff_header.wave, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Invalid WAV format (RIFF/WAVE mismatch)");
        fclose(f);
        return false;
    }

    // 3. Scan subchunks sequentially to locate the "fmt " and "data" chunks
    uint16_t num_channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;
    uint32_t data_size = 0;
    bool found_fmt = false;
    bool found_data = false;

    while (true) {
        char chunk_id[4];
        uint32_t chunk_size = 0;

        if (fread(chunk_id, 1, 4, f) != 4 || fread(&chunk_size, 1, 4, f) != 4) {
            break; // EOF or read error
        }

        if (strncmp(chunk_id, "fmt ", 4) == 0) {
            struct FmtData {
                uint16_t audio_format;
                uint16_t num_channels;
                uint32_t sample_rate;
                uint32_t byte_rate;
                uint16_t block_align;
                uint16_t bits_per_sample;
            } __attribute__((packed)) fmt;

            if (chunk_size < sizeof(fmt)) {
                ESP_LOGE(TAG, "Invalid WAV fmt chunk size: %lu", (unsigned long)chunk_size);
                break;
            }

            if (fread(&fmt, 1, sizeof(fmt), f) != sizeof(fmt)) {
                ESP_LOGE(TAG, "Failed to read fmt chunk data");
                break;
            }

            num_channels = fmt.num_channels;
            sample_rate = fmt.sample_rate;
            bits_per_sample = fmt.bits_per_sample;
            found_fmt = true;

            // Skip any remaining bytes in the fmt chunk (e.g. if format is extensible)
            if (chunk_size > sizeof(fmt)) {
                fseek(f, chunk_size - sizeof(fmt), SEEK_CUR);
            }
        }
        else if (strncmp(chunk_id, "data", 4) == 0) {
            data_size = chunk_size;
            found_data = true;
            break; // Stopped right at the start of audio data samples!
        }
        else {
            // Skip other subchunks (LIST, JUNK, etc.) ensuring padding alignment
            uint32_t seek_size = (chunk_size + 1) & ~1;
            fseek(f, seek_size, SEEK_CUR);
        }
    }

    if (!found_fmt || !found_data) {
        ESP_LOGE(TAG, "Required chunks not found (fmt: %d, data: %d)", found_fmt, found_data);
        fclose(f);
        return false;
    }

    // Print parsed audio details as requested by the user
    ESP_LOGI(TAG, "WAV metadata successfully parsed:");
    ESP_LOGI(TAG, "  Sample Rate:     %lu Hz", (unsigned long)sample_rate);
    ESP_LOGI(TAG, "  Channel Count:   %u", num_channels);
    ESP_LOGI(TAG, "  Bits per Sample: %u bits", bits_per_sample);
    ESP_LOGI(TAG, "  Data Size:       %lu bytes", (unsigned long)data_size);

    // Cache the validated parameters
    m_info.num_channels = num_channels;
    m_info.sample_rate = sample_rate;
    m_info.bits_per_sample = bits_per_sample;
    m_info.data_size = data_size;
    m_file_handle = f;
    strncpy(m_filepath, filepath, sizeof(m_filepath) - 1);
    m_filepath[sizeof(m_filepath) - 1] = '\0';
    m_playing = true;
    m_stop_requested = false;

    // 4. Create the background task now that we are sure the file is valid and ready
    BaseType_t ret = xTaskCreate(playbackTaskBridge, "wav_play_task", 4096, this, 6, &m_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create WAV playback task");
        fclose(f);
        m_file_handle = nullptr;
        m_playing = false;
        m_task_handle = nullptr;
        return false;
    }

    return true;
}

void WavPlayer::stop() {
    if (m_playing) {
        m_stop_requested = true;
        // Flush buffer to cut off audio immediately
        BufferManager::getInstance().flush(Buffers::SPK_RX_BUF);
        
        // Wait for the task to finish exiting (relaxed from 5ms to 20ms to prevent CPU spinning)
        while (m_playing) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

void WavPlayer::playbackTaskBridge(void* pvParameters) {
    static_cast<WavPlayer*>(pvParameters)->playbackTask();
}

void WavPlayer::playbackTask() {
    ESP_LOGI(TAG, "Playback task started. Streaming samples [Rate: %lu Hz, Channels: %u, Bits: %u, Size: %lu bytes]",
             (unsigned long)m_info.sample_rate, m_info.num_channels, m_info.bits_per_sample, (unsigned long)m_info.data_size);

    FILE* f = m_file_handle;
    if (f == nullptr) {
        ESP_LOGE(TAG, "Null file handle in playback task!");
        m_playing = false;
        m_task_handle = nullptr;
        vTaskDelete(NULL);
        return;
    }

    // 1. Notify the state database that a WAV file is starting, reactively triggers AudioService rate switch
    EmbeddedSysDb::getInstance().mutate([this](SystemState& s) {
        s.audio.wav_sample_rate = m_info.sample_rate;
        s.audio.wav_playing = true;
    });

    size_t samples_per_chunk = 120;
    size_t bytes_per_sample = m_info.bits_per_sample / 8;
    size_t chunk_size = samples_per_chunk * m_info.num_channels * bytes_per_sample;

    uint8_t* buffer = (uint8_t*)malloc(chunk_size);
    if (buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate WAV chunk buffer");
        fclose(f);
        m_file_handle = nullptr;
        EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
            s.audio.wav_playing = false;
        });
        m_playing = false;
        m_task_handle = nullptr;
        vTaskDelete(NULL);
        return;
    }

    auto &bm = BufferManager::getInstance();

    while (m_playing && !m_stop_requested) {
        size_t bytes_read = fread(buffer, 1, chunk_size, f);
        if (bytes_read == 0) {
            // EOF reached
            break;
        }

        size_t send_size = bytes_read;

        // Stereo to Mono downmix if needed (our pipeline outputs mono)
        if (m_info.num_channels == 2 && bytes_per_sample == 2) {
            int16_t* pcm = (int16_t*)buffer;
            size_t num_frames = bytes_read / (2 * sizeof(int16_t));
            for (size_t i = 0; i < num_frames; i++) {
                int32_t mix = ((int32_t)pcm[2 * i] + (int32_t)pcm[2 * i + 1]) / 2;
                pcm[i] = (int16_t)mix;
            }
            send_size = num_frames * sizeof(int16_t);
        }

        // Send to ringbuffer (blocks if full, which throttles this loop naturally)
        bm.send(Buffers::SPK_RX_BUF, buffer, send_size, pdMS_TO_TICKS(100));
    }

    free(buffer);
    fclose(f);
    m_file_handle = nullptr;

    // 2. Wait until the ringbuffer has been completely drained by SpeakerPlaybackTask.
    // This serves as the event-driven trigger that the audio output is fully complete.
    while (m_playing && !m_stop_requested && bm.getUsedBytes(Buffers::SPK_RX_BUF) > 0) {
        vTaskDelay(pdMS_TO_TICKS(20)); // Relaxed from 10ms to 20ms
    }

    // 3. Clear WAV playing state, which reactively restores the 16 kHz clock and re-arms wake-word
    EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
        s.audio.wav_playing = false;
    });

    m_playing = false;
    m_task_handle = nullptr;
    ESP_LOGI(TAG, "Playback task finished cleanly.");
    vTaskDelete(NULL);
}
