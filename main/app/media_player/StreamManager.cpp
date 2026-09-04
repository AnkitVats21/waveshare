#include "StreamManager.h"
#include "StorageManager.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "PlayerTypes.h"
#include "freertos/idf_additions.h"
#include "common/thread_config.h"

static const char* TAG = "StreamManager";

StreamManager::StreamManager(BufferManager::BufferId playbackId, BufferManager::BufferId storageId, StorageManager& storageMngr)
    : _bm(BufferManager::getInstance()),
      _playbackId(playbackId),
      _storageId(storageId),
      _storage(storageMngr) {}

StreamManager::~StreamManager() {
    stopStreaming();
}

bool StreamManager::beginStreaming(const char* url, bool cacheMode) {
    if (!url) return false;
    stopStreaming();

    _url = url;
    _cacheMode = cacheMode;
    _isStreaming = true;

    // Prefer allocating stack in PSRAM to conserve internal SRAM for network buffers
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        networkTaskThunk, "net_stream_task", 6 * 1024, this,
        ThreadConfig::Priority::NORMAL, &_networkTaskHandle, ThreadConfig::CORE_NETWORK,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (ret != pdPASS) {
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
    self->runStreamLoop();
    self->_networkTaskHandle = nullptr;
    vTaskDelete(NULL);
}

void StreamManager::runStreamLoop() {
    ESP_LOGI(TAG, "Network Task running on Core 0 (cacheMode=%s)", _cacheMode ? "true" : "false");
    BufferManager::BufferId targetBuf = _cacheMode ? _storageId : _playbackId;

    if (!_http.open(_url)) {
        ESP_LOGE(TAG, "Failed to connect to stream: %s", _url.c_str());
        AudioChunkHeader err_chunk = {ChunkType::ERROR, 0};
        _bm.send(targetBuf, &err_chunk, sizeof(err_chunk), portMAX_DELAY);
        _isStreaming = false;
        return;
    }

    size_t allocSize = sizeof(AudioChunkHeader) + AUDIO_CHUNK_SIZE;
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

    while (_isStreaming && _http.isConnected()) {
        int bytes_read = _http.read(payload, AUDIO_CHUNK_SIZE);
        if (bytes_read > 0) {
            header->type = ChunkType::DATA;
            header->size = bytes_read;
            ESP_LOGD(TAG, "Network chunk received: %d bytes (sent to %s)", bytes_read,
                     _cacheMode ? "STREAM_BUF" : "PLAYER_BUF");

            // Send chunk to targetBuf (blocks with timeout to throttle socket reads)
            bool sent = false;
            while (_isStreaming && !sent) {
                sent = _bm.send(targetBuf, net_buf, sizeof(AudioChunkHeader) + bytes_read, pdMS_TO_TICKS(100));
            }
        } else if (bytes_read == 0) {
            ESP_LOGI(TAG, "Network stream completed naturally");
            break;
        } else {
            ESP_LOGE(TAG, "Network stream read error!");
            error_occurred = true;
            break;
        }
    }

    // Wrap-up and notify downstream task (AudioEngine or StorageManager)
    if (error_occurred) {
        header->type = ChunkType::ERROR;
        header->size = 0;
        _bm.send(targetBuf, net_buf, sizeof(AudioChunkHeader), portMAX_DELAY);
    } else {
        header->type = ChunkType::EOF_STREAM;
        header->size = 0;
        _bm.send(targetBuf, net_buf, sizeof(AudioChunkHeader), portMAX_DELAY);
    }

    ESP_LOGI(TAG, "Network Task wrap-up: error=%d", error_occurred ? 1 : 0);
    heap_caps_free(net_buf);
    _http.close();
    _isStreaming = false;
    ESP_LOGI(TAG, "Network Task exiting");
}
