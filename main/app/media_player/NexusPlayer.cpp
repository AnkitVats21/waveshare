#include "NexusPlayer.h"
#include "esp_log.h"
#include <cstring>
#include "app/audio/SpeakerPlayback.h"

static const char* TAG = "NexusPlayer";

// Define the playback and storage buffers using the BufferManager macro
// We use RINGBUF_TYPE_NOSPLIT as specified in the design document
DEFINE_BUFFER_WITH_TYPE(PLAYER_BUF, "player_buf", 512 * 1024, RINGBUF_TYPE_NOSPLIT)
DEFINE_BUFFER_WITH_TYPE(STREAM_BUF, "stream_buf", 256 * 1024, RINGBUF_TYPE_NOSPLIT)

NexusPlayer& NexusPlayer::getInstance() {
    static NexusPlayer instance(Buffers::PLAYER_BUF, Buffers::STREAM_BUF);
    return instance;
}

NexusPlayer::NexusPlayer(BufferManager::BufferId playbackId, BufferManager::BufferId storageId)
    : _savedPcmBuffer(nullptr),
      _savedPcmLen(0),
      _playbackId(playbackId),
      _storageId(storageId),
      _storageManager(playbackId, storageId),
      _streamManager(playbackId, storageId, _storageManager),
      _audioEngine(playbackId, Buffers::SPK_RX_BUF) {}

NexusPlayer::~NexusPlayer() {
    stop();
    if (_savedPcmBuffer != nullptr) {
        heap_caps_free(_savedPcmBuffer);
        _savedPcmBuffer = nullptr;
    }
}

bool NexusPlayer::begin() {
    ESP_LOGI(TAG, "NexusPlayer initialization");
    
    size_t spk_buf_size = BufferManager::getInstance().size(Buffers::SPK_RX_BUF);
    if (spk_buf_size == 0) {
        spk_buf_size = 512 * 1024;
    }
    _savedPcmBuffer = (uint8_t*)heap_caps_malloc(spk_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_savedPcmBuffer) {
        ESP_LOGE(TAG, "Failed to allocate saved PCM buffer in SPIRAM");
    }
    _savedPcmLen = 0;

    return _audioEngine.initialize(32000, 1);
}

void NexusPlayer::play(const char* songId, const char* downloadUrl) {
    if (!songId || !downloadUrl) {
        ESP_LOGE(TAG, "Invalid play arguments");
        return;
    }

    ESP_LOGI(TAG, "Play requested for songId: %s, url: %s", songId, downloadUrl);

    // If currently playing, stop it first
    if (_state != STATE_IDLE) {
        stop();
    }

    strncpy(_activeSongId, songId, sizeof(_activeSongId) - 1);
    _activeSongId[sizeof(_activeSongId) - 1] = '\0';

    // Flush all buffers before starting new session
    BufferManager::getInstance().flush(_playbackId);
    BufferManager::getInstance().flush(_storageId);
    BufferManager::getInstance().flush(Buffers::SPK_RX_BUF);

    if (_storageManager.fileExists(songId)) {
        ESP_LOGI(TAG, "Cache Hit! Playing local file for songId: %s", songId);
        _state = STATE_LOCAL_PLAYBACK;

        // Start decoding engine
        _audioEngine.start();

        // Open local file for reading. Spawns Reader Task.
        if (!_storageManager.openFileForReading(songId)) {
            ESP_LOGE(TAG, "Failed to open local file for reading");
            stopActivePipelines();
            _state = STATE_IDLE;
            return;
        }
    } else {
        ESP_LOGI(TAG, "Cache Miss! Downloading and streaming songId: %s", songId);
        _state = STATE_STREAMING_AND_CACHING;

        // Start decoding engine
        _audioEngine.start();

        // Open temp file for caching (writes stream to it and reads progressively)
        // Spawns Writer and Reader tasks.
        if (!_storageManager.openFileForCaching(songId)) {
            ESP_LOGE(TAG, "Failed to open file for caching");
            stopActivePipelines();
            _state = STATE_IDLE;
            return;
        }

        // Start downloading HTTP stream chunk-by-chunk. Spawns Network Task.
        if (!_streamManager.beginStreaming(downloadUrl)) {
            ESP_LOGE(TAG, "Failed to start streaming");
            stopActivePipelines();
            _state = STATE_IDLE;
            return;
        }
    }
}

void NexusPlayer::pause() {
    if (_state == STATE_STREAMING_AND_CACHING || _state == STATE_LOCAL_PLAYBACK) {
        ESP_LOGI(TAG, "Pausing playback");
        _audioEngine.pause();

        // Give the audio engine a tiny delay to yield if it was actively running
        vTaskDelay(pdMS_TO_TICKS(5));

        // Save the current contents of SPK_RX_BUF to the SPIRAM buffer
        _savedPcmLen = 0;
        if (_savedPcmBuffer != nullptr) {
            auto &bm = BufferManager::getInstance();
            size_t rx_bytes = 0;
            size_t max_size = bm.size(Buffers::SPK_RX_BUF);
            if (max_size == 0) max_size = 512 * 1024;
            
            while (true) {
                void* rx_ptr = bm.receive(Buffers::SPK_RX_BUF, &rx_bytes, 0, 512 * 1024);
                if (rx_ptr == nullptr || rx_bytes == 0) {
                    break;
                }
                if (_savedPcmLen + rx_bytes <= max_size) {
                    memcpy(_savedPcmBuffer + _savedPcmLen, rx_ptr, rx_bytes);
                    _savedPcmLen += rx_bytes;
                } else {
                    ESP_LOGE(TAG, "Saved PCM buffer overflow!");
                }
                bm.returnItem(Buffers::SPK_RX_BUF, rx_ptr);
            }
            ESP_LOGI(TAG, "Saved %zu bytes of PCM data from SPK_RX_BUF", _savedPcmLen);
        }

        _state = STATE_PAUSED;
    }
}

void NexusPlayer::resume() {
    if (_state == STATE_PAUSED) {
        ESP_LOGI(TAG, "Resuming playback");

        // Restore the saved PCM data back to SPK_RX_BUF
        if (_savedPcmBuffer != nullptr && _savedPcmLen > 0) {
            auto &bm = BufferManager::getInstance();
            bool sent = bm.send(Buffers::SPK_RX_BUF, _savedPcmBuffer, _savedPcmLen, pdMS_TO_TICKS(100));
            if (sent) {
                ESP_LOGI(TAG, "Restored %zu bytes of PCM data to SPK_RX_BUF", _savedPcmLen);
            } else {
                ESP_LOGE(TAG, "Failed to restore PCM data to SPK_RX_BUF");
            }
            _savedPcmLen = 0;
        }

        _audioEngine.resume();
        if (_streamManager.isStreaming()) {
            _state = STATE_STREAMING_AND_CACHING;
        } else {
            _state = STATE_LOCAL_PLAYBACK;
        }
    }
}

void NexusPlayer::stop() {
    if (_state == STATE_IDLE) {
        return;
    }
    ESP_LOGI(TAG, "Stopping playback and active pipelines");
    stopActivePipelines();
    _savedPcmLen = 0;
    _state = STATE_IDLE;
    _activeSongId[0] = '\0';
}

void NexusPlayer::stopActivePipelines() {
    // 1. Stop streaming from network (kills HTTP connection and net task)
    _streamManager.stopStreaming();

    // 2. Unblock AudioEngine decoder task from waiting on PLAYER_BUF
    // We send an EOF chunk to PLAYER_BUF to unblock the receive
    AudioChunkHeader eof_header = {ChunkType::EOF_STREAM, 0};
    BufferManager::getInstance().send(_playbackId, &eof_header, sizeof(eof_header));

    // Also send to storageId just in case writer is waiting
    BufferManager::getInstance().send(_storageId, &eof_header, sizeof(eof_header));

    // 3. Stop AudioEngine (kills decoder task)
    _audioEngine.stop();

    // 4. Stop and clean up SD Reader/Writer tasks and active file streams
    _storageManager.closeActiveFile();

    // 5. Flush all buffers
    BufferManager::getInstance().flush(_playbackId);
    BufferManager::getInstance().flush(_storageId);
    BufferManager::getInstance().flush(Buffers::SPK_RX_BUF);
}
