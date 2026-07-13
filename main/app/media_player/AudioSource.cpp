#include "AudioSource.h"
#include "OpusPlayer.h"
#include "services/BufferManager.h"
#include "common/thread_config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ── AudioSource Base Implementation ──────────────────────────────────────────

AudioSource::AudioSource(const char* name, uint32_t stack_size, UBaseType_t priority, BaseType_t core_id)
    : TaskBase({name, stack_size, priority, core_id}) {}

void AudioSource::stopSource() {
    m_stop.store(true);
    this->stop();
}

void AudioSource::resetSource() {
    m_stop.store(false);
}

// ── SdCardAudioSource Implementation ──────────────────────────────────────────

SdCardAudioSource::SdCardAudioSource()
    : AudioSource("opus_sd_source", 4096, ThreadConfig::Priority::LOW, ThreadConfig::CORE_NETWORK) {
    const size_t alloc_size = sizeof(AudioChunkHeader) + AUDIO_CHUNK_SIZE;
    m_chunk_buf = (uint8_t*)heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (m_chunk_buf == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate file read buffer in PSRAM");
    }
}

SdCardAudioSource::~SdCardAudioSource() {
    if (m_chunk_buf) {
        heap_caps_free(m_chunk_buf);
        m_chunk_buf = nullptr;
    }
}

bool SdCardAudioSource::setFilePath(const char* path) {
    if (path == nullptr) return false;
    m_path = path;
    return true;
}

void SdCardAudioSource::resetSource() {
    AudioSource::resetSource();
}

void SdCardAudioSource::run() {
    ESP_LOGI(TAG, "SdCardAudioSource task started for file: %s", m_path.c_str());

    if (m_chunk_buf == nullptr) {
        ESP_LOGE(TAG, "Chunk buffer not allocated!");
        AudioChunkHeader err_header = { ChunkType::ERROR, 0 };
        BufferManager::getInstance().send(Buffers::OPUS_COMP_BUF, &err_header, sizeof(err_header), portMAX_DELAY);
        return;
    }

    FILE* f = fopen(m_path.c_str(), "rb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "Failed to open file: %s", m_path.c_str());
        AudioChunkHeader err_header = { ChunkType::ERROR, 0 };
        BufferManager::getInstance().send(Buffers::OPUS_COMP_BUF, &err_header, sizeof(err_header), portMAX_DELAY);
        return;
    }

    AudioChunkHeader* header = reinterpret_cast<AudioChunkHeader*>(m_chunk_buf);

    while (!m_stop.load()) {
        size_t bytes_read = fread(m_chunk_buf + sizeof(AudioChunkHeader), 1, AUDIO_CHUNK_SIZE, f);
        if (bytes_read == 0) {
            if (ferror(f)) {
                ESP_LOGE(TAG, "Error reading file: %s", m_path.c_str());
                header->type = ChunkType::ERROR;
                header->size = 0;
            } else {
                ESP_LOGI(TAG, "Reached EOF of file: %s", m_path.c_str());
                header->type = ChunkType::EOF_STREAM;
                header->size = 0;
            }
            break;
        }

        header->type = ChunkType::DATA;
        header->size = bytes_read;

        // Write the header + data as a single item into the shared ring buffer
        bool sent = BufferManager::getInstance().send(
            Buffers::OPUS_COMP_BUF,
            m_chunk_buf,
            sizeof(AudioChunkHeader) + bytes_read,
            pdMS_TO_TICKS(100) // Block up to 100ms if full, checking stop flag periodically
        );
        uint32_t blocked_ms = 0;
        while (!m_stop.load() && !sent) {
            sent = BufferManager::getInstance().send(
                Buffers::OPUS_COMP_BUF,
                m_chunk_buf,
                sizeof(AudioChunkHeader) + bytes_read,
                pdMS_TO_TICKS(100)
            );
            if (!sent) {
                blocked_ms += 100;
                if (blocked_ms >= 5000) {
                    ESP_LOGW(TAG, "Send blocked for over 5 seconds. Potential deadlock or slow consumer!");
                    blocked_ms = 0;
                }
            }
        }
    }

    fclose(f);

    // Send explicit end-of-stream or error event
    if (!m_stop.load()) {
        BufferManager::getInstance().send(Buffers::OPUS_COMP_BUF, header, sizeof(AudioChunkHeader), portMAX_DELAY);
    }
    ESP_LOGI(TAG, "SdCardAudioSource task finished.");
}

// ── HttpAudioSource Implementation ──────────────────────────────────────────

HttpAudioSource::HttpAudioSource()
    : AudioSource("opus_http_source", 4096, ThreadConfig::Priority::LOW, ThreadConfig::CORE_NETWORK) {
    const size_t alloc_size = sizeof(AudioChunkHeader) + AUDIO_CHUNK_SIZE;
    m_chunk_buf = (uint8_t*)heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (m_chunk_buf == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate Http chunk buffer in PSRAM");
    }
}

HttpAudioSource::~HttpAudioSource() {
    if (m_chunk_buf) {
        heap_caps_free(m_chunk_buf);
        m_chunk_buf = nullptr;
    }
}

bool HttpAudioSource::setUrl(const char* url) {
    if (url == nullptr) return false;
    m_url = url;
    return true;
}

void HttpAudioSource::resetSource() {
    AudioSource::resetSource();
}

void HttpAudioSource::run() {
    ESP_LOGI(TAG, "HttpAudioSource task started (stub).");
    if (!m_stop.load()) {
        AudioChunkHeader eof_header = { ChunkType::EOF_STREAM, 0 };
        BufferManager::getInstance().send(Buffers::OPUS_COMP_BUF, &eof_header, sizeof(eof_header), portMAX_DELAY);
    }
}
