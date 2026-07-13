#include "StreamManager.h"
#include "StorageManager.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "PlayerTypes.h"


static const char* TAG = "StreamManager";

StreamManager::StreamManager(BufferManager::BufferId playbackId, BufferManager::BufferId storageId, StorageManager& storageMngr)
    : _bm(BufferManager::getInstance()),
      _playbackId(playbackId),
      _storageId(storageId),
      _storage(storageMngr) {}

StreamManager::~StreamManager() {
    stopStreaming();
}

bool StreamManager::beginStreaming(const char* url) {
    if (!url) return false;
    stopStreaming();

    _url = url;
    _isStreaming = true;

    xTaskCreatePinnedToCore(
        networkTaskThunk, "net_stream_task", 4096, this,
        5, &_networkTaskHandle, 0
    );

    if (!_networkTaskHandle) {
        ESP_LOGE(TAG, "Failed to spawn net_stream_task");
        _isStreaming = false;
        return false;
    }

    return true;
}

void StreamManager::stopStreaming() {
    if (_isStreaming || _networkTaskHandle != nullptr) {
        ESP_LOGI(TAG, "Stopping streaming...");
        _isStreaming = false;
        _http.close();

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
    ESP_LOGI(TAG, "Network Task running on Core 0");

    if (!_http.open(_url)) {
        ESP_LOGE(TAG, "Failed to connect to stream: %s", _url.c_str());
        AudioChunkHeader err_chunk = {ChunkType::ERROR, 0};
        _bm.send(_storageId, &err_chunk, sizeof(err_chunk), portMAX_DELAY);
        _isStreaming = false;
        return;
    }

    size_t allocSize = sizeof(AudioChunkHeader) + AUDIO_CHUNK_SIZE;
    uint8_t* net_buf = static_cast<uint8_t*>(heap_caps_malloc(allocSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!net_buf) {
        ESP_LOGE(TAG, "Failed to allocate network read buffer in PSRAM!");
        AudioChunkHeader err_chunk = {ChunkType::ERROR, 0};
        _bm.send(_storageId, &err_chunk, sizeof(err_chunk), portMAX_DELAY);
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
            ESP_LOGD(TAG, "Network chunk received: %d bytes", bytes_read);

            // Send chunk to STREAM_BUF (blocks with timeout to throttle socket reads)
            bool sent = false;
            while (_isStreaming && !sent) {
                sent = _bm.send(_storageId, net_buf, sizeof(AudioChunkHeader) + bytes_read, pdMS_TO_TICKS(100));
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

    // Wrap-up and notify downstream task (StorageManager)
    if (error_occurred) {
        header->type = ChunkType::ERROR;
        header->size = 0;
        _bm.send(_storageId, net_buf, sizeof(AudioChunkHeader), portMAX_DELAY);
    } else {
        header->type = ChunkType::EOF_STREAM;
        header->size = 0;
        _bm.send(_storageId, net_buf, sizeof(AudioChunkHeader), portMAX_DELAY);
    }

    ESP_LOGI(TAG, "Network Task wrap-up: error=%d", error_occurred ? 1 : 0);
    heap_caps_free(net_buf);
    _http.close();
    _isStreaming = false;
    ESP_LOGI(TAG, "Network Task exiting");
}
