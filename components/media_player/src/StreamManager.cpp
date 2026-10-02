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
// A connection lost before the end is reopened at the byte where it stopped:
// the first try at once, then 1 s and 2 s later.
static constexpr int MAX_RECONNECTS = 3;

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

bool StreamManager::beginStreaming(const char* url, bool cacheMode, const char* trackId) {
    return start(url, cacheMode, 0, false, 0, trackId);
}

bool StreamManager::beginStreamingFrom(const char* url, uint32_t byteOffset, bool cacheMode, const char* trackId) {
    return start(url, cacheMode, byteOffset, false, 0, trackId);
}

bool StreamManager::beginStreamingAt(const char* url, uint32_t targetMs, const char* trackId) {
    return start(url, false, 0, true, targetMs, trackId);
}

bool StreamManager::start(const char* url, bool cacheMode, uint32_t byteOffset, bool atTime, uint32_t targetMs,
                          const char* trackId) {
    if (!url) return false;
    stopStreaming();

    if (_cuesUrl != url) {
        _cues.clear();
        _cuesUrl = url;
        _indexKnown = false;
        _renewedUrl.clear();
        _redirectedUrl.clear();
    }
    _trackId = trackId ? trackId : "";
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
    if (!openStream(_startByteOffset)) {
        ESP_LOGE(TAG, "Failed to connect to stream (offset=%u, status=%d)",
                 (unsigned int)_startByteOffset, _http.lastStatus());
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
    // Where the next byte comes from, and where the file ends (0: unknown).
    uint32_t nextByte = _startByteOffset;
    const int64_t bodyLen = _http.contentLength();
    const uint32_t endByte = bodyLen > 0 ? _startByteOffset + static_cast<uint32_t>(bodyLen) : 0;
    int reconnects = 0;

    while (_isStreaming) {
        if (_held) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        int bytes_read = _http.isConnected() ? _http.read(payload, readSize) : -1;
        if (bytes_read > 0) {
            nextByte += bytes_read;
            reconnects = 0;
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
        } else if (bytes_read == 0 && (endByte == 0 || nextByte >= endByte)) {
            ESP_LOGI(TAG, "Network stream completed naturally");
            completed = true;
            break;
        } else {
            // Closed or failed before the end: the server drops a connection
            // left idle by a pause after a few minutes, or Wi-Fi dropped.
            // Reopen at the next byte; the decoder (and a file being saved)
            // gets the rest of the same file, as if nothing happened.
            if (!_isStreaming) break;
            if (reconnects >= MAX_RECONNECTS) {
                ESP_LOGE(TAG, "Network stream read error! Gave up after %d reconnects at byte %u of %u",
                         reconnects, (unsigned)nextByte, (unsigned)endByte);
                error_occurred = true;
                break;
            }
            ++reconnects;
            ESP_LOGW(TAG, "Connection lost at byte %u of %u (%s); reconnecting (%d/%d)", (unsigned)nextByte,
                     (unsigned)endByte, bytes_read == 0 ? "closed early" : "read error", reconnects,
                     MAX_RECONNECTS);
            _http.close();
            if (!waitWhileStreaming((reconnects - 1) * 1000)) break;
            const int64_t t0 = esp_timer_get_time();
            if (openStream(nextByte)) {
                ESP_LOGI(TAG, "Reconnected at byte %u in %lld ms", (unsigned)nextByte,
                         (long long)((esp_timer_get_time() - t0) / 1000));
            }
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

bool StreamManager::waitWhileStreaming(uint32_t ms) {
    for (uint32_t waited = 0; waited < ms && _isStreaming; waited += 50) vTaskDelay(pdMS_TO_TICKS(50));
    return _isStreaming;
}

bool StreamManager::openAndRemember(const std::string& url, uint32_t offset) {
    if (!_http.open(url, offset)) return false;
    if (!_http.finalUrl().empty() && _http.finalUrl() != _redirectedUrl) {
        _redirectedUrl = _http.finalUrl();
        ESP_LOGI(TAG, "Remembering the redirect for this track");
    }
    return true;
}

bool StreamManager::openStream(uint32_t offset) {
    if (!_redirectedUrl.empty()) {
        ESP_LOGI(TAG, "Using the remembered redirect");
        if (_http.open(_redirectedUrl, offset)) return true;
        if (!_isStreaming) return false;
        ESP_LOGW(TAG, "Remembered redirect failed (status %d); using the track's URL", _http.lastStatus());
        _redirectedUrl.clear();
    }
    if (openAndRemember(currentUrl(), offset)) return true;
    // googlevideo answers an expired (or revoked) URL with 403.
    const int status = _http.lastStatus();
    if ((status != 403 && status != 410) || !_isStreaming || !renewUrl()) return false;
    return openAndRemember(currentUrl(), offset);
}

bool StreamManager::renewUrl() {
    if (!_urlRenewer || _trackId.empty()) return false;
    ESP_LOGW(TAG, "Stream URL rejected (status %d): resolving %s again", _http.lastStatus(), _trackId.c_str());
    const int64_t t0 = esp_timer_get_time();
    std::string fresh;
    if (!_urlRenewer(_trackId, fresh, [this] { return !_isStreaming; }) || fresh.empty()) {
        ESP_LOGE(TAG, "Could not renew the stream URL for %s", _trackId.c_str());
        return false;
    }
    // Byte offsets (the index, the reconnect point, a file being saved) hold
    // only for the same file; a different format or size can't continue.
    const double oldLen = urlNumberParam(currentUrl().c_str(), "clen");
    const double newLen = urlNumberParam(fresh.c_str(), "clen");
    if (oldLen > 0 && newLen != oldLen) {
        ESP_LOGE(TAG, "Renewed URL is a different file (%.0f bytes, was %.0f)", newLen, oldLen);
        return false;
    }
    _renewedUrl = std::move(fresh);
    _redirectedUrl.clear();
    ESP_LOGI(TAG, "Stream URL renewed in %lld ms", (long long)((esp_timer_get_time() - t0) / 1000));
    return true;
}

void StreamManager::fetchIndex() {
    const int64_t t0 = esp_timer_get_time();
    if (!openStream(0)) {
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
