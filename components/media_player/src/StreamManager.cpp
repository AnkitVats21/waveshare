#include "StreamManager.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "PlayerTypes.h"
#include "freertos/idf_additions.h"
#include "common/thread_config.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

static const char* TAG = "StreamManager";

// Each read blocks until it is full, and stopStreaming (a seek) waits for the
// read in progress: 8 KB takes ~100 ms at the ~70 KB/s the TCP window allows.
// Saving keeps full chunks: the SD writer syncs once per chunk.
static constexpr size_t NET_READ_SIZE = 8 * 1024;

StreamManager::StreamManager(BufferManager::BufferId playbackId, BufferManager::BufferId storageId)
    : _bm(BufferManager::getInstance()),
      _playbackId(playbackId),
      _storageId(storageId) {}

StreamManager::~StreamManager() {
    stopStreaming();
}

double StreamManager::urlNumberParam(const char* url, const char* key) {
    if (!url) return 0;
    const size_t klen = strlen(key);
    for (const char* p = strchr(url, '?'); p; p = strchr(p + 1, '&')) {
        if (strncmp(p + 1, key, klen) == 0 && p[1 + klen] == '=') return strtod(p + 2 + klen, nullptr);
    }
    return 0;
}

bool StreamManager::beginStreaming(const char* url, bool cacheMode) {
    return start(url, cacheMode, 0, false, 0);
}

bool StreamManager::beginStreamingFrom(const char* url, uint32_t byteOffset, bool cacheMode) {
    return start(url, cacheMode, byteOffset, false, 0);
}

bool StreamManager::beginStreamingAt(const char* url, uint32_t targetMs) {
    return start(url, false, 0, true, targetMs);
}

bool StreamManager::start(const char* url, bool cacheMode, uint32_t byteOffset, bool atTime, uint32_t targetMs) {
    if (!url) return false;
    stopStreaming();

    if (_cuesUrl != url) {
        _cues.clear();
        _cuesUrl = url;
        _indexKnown = false;
    }
    _startByteOffset = byteOffset;
    _startAtTime = atTime;
    _targetMs = targetMs;
    _url = url;
    _cacheMode = cacheMode;
    _isStreaming = true;

    // Prefer allocating stack in PSRAM to conserve internal SRAM for network buffers
    _taskCreatedWithCaps = true;
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        networkTaskThunk, "net_stream_task", 6 * 1024, this,
        ThreadConfig::Priority::NORMAL, &_networkTaskHandle, ThreadConfig::CORE_NETWORK,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (ret != pdPASS) {
        _taskCreatedWithCaps = false;
        ret = xTaskCreatePinnedToCore(
            networkTaskThunk, "net_stream_task", 4096, this,
            ThreadConfig::Priority::NORMAL, &_networkTaskHandle, ThreadConfig::CORE_NETWORK
        );
    }

    if (ret != pdPASS || !_networkTaskHandle) {
        ESP_LOGE(TAG, "Failed to spawn net_stream_task (ret=%d)", (int)ret);
        _isStreaming = false;
        return false;
    }

    return true;
}

void StreamManager::stopStreaming() {
    if (_isStreaming || _networkTaskHandle != nullptr) {
        ESP_LOGI(TAG, "Stopping streaming...");
        _isStreaming = false;

        // Wait for network task to finish cleanly and close its own connection
        while (_networkTaskHandle != nullptr) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        ESP_LOGI(TAG, "Streaming stopped completely");
    }
}

void StreamManager::networkTaskThunk(void* pvParameters) {
    StreamManager* self = static_cast<StreamManager*>(pvParameters);
    bool withCaps = self->_taskCreatedWithCaps;
    self->runStreamLoop();
    self->_networkTaskHandle = nullptr;
    if (withCaps) {
        vTaskDeleteWithCaps(NULL);
    } else {
        vTaskDelete(NULL);
    }
}

void StreamManager::runStreamLoop() {
    ESP_LOGI(TAG, "Network Task running on Core 0 (cacheMode=%s, offset=%u)",
             _cacheMode ? "true" : "false", (unsigned int)_startByteOffset);
    BufferManager::BufferId targetBuf = _cacheMode ? _storageId : _playbackId;

    if (_startAtTime) {
        const int64_t t0 = esp_timer_get_time();
        if (!_indexKnown) fetchIndex();
        const char* how = "";
        _startByteOffset = resolveOffset(_targetMs, how);
        ESP_LOGI(TAG, "Start at %u ms: byte %u (%s), found in %lld us", (unsigned)_targetMs,
                 (unsigned)_startByteOffset, how, (long long)(esp_timer_get_time() - t0));
        if (!_isStreaming) {   // stopped while fetching the index
            ESP_LOGI(TAG, "Network Task exiting");
            return;
        }
    }
    // From the start the header arrives first: keep it until the index is read.
    if (_startByteOffset == 0 && !_indexKnown) _headWant = 4096;

    const int64_t connectStart = esp_timer_get_time();
    if (!_http.open(_url, _startByteOffset)) {
        ESP_LOGE(TAG, "Failed to connect to stream (offset=%u): %s",
                 (unsigned int)_startByteOffset, _url.c_str());
        AudioChunkHeader err_chunk = {ChunkType::ERROR, 0};
        _bm.send(targetBuf, &err_chunk, sizeof(err_chunk), portMAX_DELAY);
        _isStreaming = false;
        return;
    }

    ESP_LOGI(TAG, "Connected in %lld ms", (long long)((esp_timer_get_time() - connectStart) / 1000));

    const size_t readSize = _cacheMode ? AUDIO_CHUNK_SIZE : NET_READ_SIZE;
    size_t allocSize = sizeof(AudioChunkHeader) + readSize;
    uint8_t* net_buf = static_cast<uint8_t*>(heap_caps_malloc(allocSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!net_buf) {
        ESP_LOGE(TAG, "Failed to allocate network read buffer in PSRAM!");
        AudioChunkHeader err_chunk = {ChunkType::ERROR, 0};
        _bm.send(targetBuf, &err_chunk, sizeof(err_chunk), portMAX_DELAY);
        _http.close();
        _isStreaming = false;
        return;
    }

    AudioChunkHeader* header = reinterpret_cast<AudioChunkHeader*>(net_buf);
    uint8_t* payload = net_buf + sizeof(AudioChunkHeader);

    bool error_occurred = false;
    bool completed = false;

    while (_isStreaming && _http.isConnected()) {
        int bytes_read = _http.read(payload, readSize);
        if (bytes_read > 0) {
            if (_headWant > 0) captureHead(payload, bytes_read);
            header->type = ChunkType::DATA;
            header->size = bytes_read;
            ESP_LOGD(TAG, "Network chunk received: %d bytes (sent to %s)", bytes_read,
                     _cacheMode ? "STREAM_BUF" : "PLAYER_BUF");

            // Send chunk to targetBuf (blocks with timeout to throttle socket reads).
            // The ring is usually full; the short timeout lets a stop get through fast.
            bool sent = false;
            while (_isStreaming && !sent) {
                sent = _bm.send(targetBuf, net_buf, sizeof(AudioChunkHeader) + bytes_read, pdMS_TO_TICKS(20));
            }
        } else if (bytes_read == 0) {
            ESP_LOGI(TAG, "Network stream completed naturally");
            completed = true;
            break;
        } else {
            ESP_LOGE(TAG, "Network stream read error!");
            error_occurred = true;
            break;
        }
    }

    // Wrap-up and notify downstream task (AudioEngine or StorageManager).
    // Bounded send: never block teardown on a full downstream ring buffer. If the
    // consumer is being torn down it will flush anyway, and a dropped EOF marker is
    // harmless. A portMAX_DELAY here previously deadlocked shutdown: stopStreaming()
    // busy-waits for this task to exit while the consumer is stopped/flushed only
    // *after* stopStreaming() returns.
    //
    // Stopped before the end: send nothing. The consumer is being torn down,
    // and an EOF would tell StorageManager the download finished, committing a
    // partial file to the cache as if it were the whole track.
    if (_isStreaming || completed || error_occurred) {
        header->type = error_occurred ? ChunkType::ERROR : ChunkType::EOF_STREAM;
        header->size = 0;
        for (int i = 0; i < 50; ++i) {
            if (_bm.send(targetBuf, net_buf, sizeof(AudioChunkHeader), pdMS_TO_TICKS(20))) break;
            if (!_isStreaming) break;  // teardown in progress - consumer will flush
        }
    }

    ESP_LOGI(TAG, "Network Task wrap-up: error=%d", error_occurred ? 1 : 0);
    endCapture();
    heap_caps_free(net_buf);
    _http.close();
    _isStreaming = false;
    ESP_LOGI(TAG, "Network Task exiting");
}

void StreamManager::fetchIndex() {
    const int64_t t0 = esp_timer_get_time();
    if (!_http.open(_url, 0)) {
        ESP_LOGW(TAG, "Index request failed");
        return;
    }
    size_t want = 4096;
    size_t len = 0;
    uint8_t* head = nullptr;
    while (_isStreaming) {
        uint8_t* grown = static_cast<uint8_t*>(heap_caps_realloc(head, want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!grown) break;
        head = grown;
        while (len < want && _isStreaming) {
            const int n = _http.read(head + len, want - len);
            if (n <= 0) break;
            len += n;
        }
        const size_t asked = want;
        if (takeIndex(head, len, want, "request") || len < asked) break;
    }
    _http.close();
    heap_caps_free(head);
    ESP_LOGI(TAG, "Index request: %u bytes in %lld us", (unsigned)len, (long long)(esp_timer_get_time() - t0));
}

bool StreamManager::takeIndex(const uint8_t* head, size_t len, size_t& want, const char* how) {
    Media::WebmCues index = Media::parseWebmCues(head, len);
    if (index.status == Media::CuesStatus::NeedMore && index.need > len) {
        want = index.need;
        return false;
    }
    // Cut off (a short read): leave it unknown so the next start tries again.
    _indexKnown = index.status != Media::CuesStatus::NeedMore;
    if (index.status == Media::CuesStatus::Found) _cues = std::move(index.cues);
    const char* status = index.status == Media::CuesStatus::Found      ? "found"
                         : index.status == Media::CuesStatus::NotFound ? "none"
                         : index.status == Media::CuesStatus::NeedMore ? "cut off"
                                                                       : "not WebM";
    ESP_LOGI(TAG, "Seek index (%s): %s, %u cues, %u header bytes", how, status, (unsigned)_cues.size(),
             (unsigned)len);
    return true;
}

void StreamManager::captureHead(const uint8_t* data, size_t len) {
    while (_headWant > 0 && len > 0) {
        if (!_head) {
            _head = static_cast<uint8_t*>(heap_caps_malloc(_headWant, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!_head) {
                endCapture();
                return;
            }
        }
        const size_t take = std::min(len, _headWant - _headLen);
        memcpy(_head + _headLen, data, take);
        _headLen += take;
        data += take;
        len -= take;
        if (_headLen < _headWant) return;

        size_t want = _headWant;
        if (takeIndex(_head, _headLen, want, "stream")) {
            endCapture();
            return;
        }
        uint8_t* grown = static_cast<uint8_t*>(heap_caps_realloc(_head, want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!grown) {
            endCapture();
            return;
        }
        _head = grown;
        _headWant = want;
    }
}

void StreamManager::endCapture() {
    heap_caps_free(_head);
    _head = nullptr;
    _headLen = 0;
    _headWant = 0;
}

uint32_t StreamManager::resolveOffset(uint32_t targetMs, const char*& how) {
    if (!_cues.empty()) {
        how = "index";
        return Media::cueAtOrBefore(_cues, targetMs).offset;
    }
    // No index: estimate from the average bitrate, aiming 5 s early so the
    // decoder starts before the target and skips forward to it.
    constexpr uint32_t EARLY_MS = 5000;
    const double durMs = urlNumberParam(_url.c_str(), "dur") * 1000;
    const double size = urlNumberParam(_url.c_str(), "clen");
    if (durMs <= 0 || size <= 0) {
        how = "no index or length";
        return 0;
    }
    how = "estimate";
    const uint32_t aim = targetMs > EARLY_MS ? targetMs - EARLY_MS : 0;
    return static_cast<uint32_t>(std::min(size, aim * size / durMs));
}
