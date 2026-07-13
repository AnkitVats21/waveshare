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
    : _playbackId(playbackId),
      _storageId(storageId),
      _storageManager(playbackId, storageId),
      _streamManager(playbackId, storageId, _storageManager),
      _audioEngine(playbackId, Buffers::SPK_RX_BUF) {}

NexusPlayer::~NexusPlayer() {
    stop();
}

bool NexusPlayer::begin() {
    ESP_LOGI(TAG, "NexusPlayer initialization");
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
        _state = STATE_PAUSED;
    }
}

void NexusPlayer::resume() {
    if (_state == STATE_PAUSED) {
        ESP_LOGI(TAG, "Resuming playback");
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
