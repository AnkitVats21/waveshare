#include "NexusPlayer.h"
#include "esp_log.h"
#include <cstring>
#include "app/audio/SpeakerPlayback.h"
#include "app/audio/AudioPipelineManager.h"
#include "app/audio/BtSpeakerPlaybackTask.h"
#include "common/thread_config.h"
#include "app/mqtt/MqttService.h"

static const char* TAG = "NexusPlayer";

// Define the playback and storage buffers using the BufferManager macro
DEFINE_BUFFER_WITH_TYPE(PLAYER_BUF, "player_buf", 512 * 1024, RINGBUF_TYPE_NOSPLIT)
DEFINE_BUFFER_WITH_TYPE(STREAM_BUF, "stream_buf", 256 * 1024, RINGBUF_TYPE_NOSPLIT)

// RAII helper to handle recursive mutex locking
class PlayerLock {
public:
    explicit PlayerLock(SemaphoreHandle_t mutex) : _mutex(mutex) {
        if (_mutex) {
            xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
        }
    }
    ~PlayerLock() {
        if (_mutex) {
            xSemaphoreGiveRecursive(_mutex);
        }
    }
private:
    SemaphoreHandle_t _mutex;
};

NexusPlayer& NexusPlayer::getInstance() {
    static NexusPlayer instance(Buffers::PLAYER_BUF, Buffers::STREAM_BUF);
    return instance;
}

NexusPlayer::NexusPlayer(BufferManager::BufferId playbackId, BufferManager::BufferId storageId)
    : ReactorTask({
          "nexus_player",
          ThreadConfig::StackSize::STACK_MEDIA,
          ThreadConfig::Priority::GEMINI_PROTOCOL, // Raised to 7 to preempt AssistantService (6) on Core 0
          ThreadConfig::CORE_NETWORK,
          COMP::ASSISTANT
      }),
      _playbackId(playbackId),
      _storageId(storageId),
      _storageManager(playbackId, storageId),
      _streamManager(playbackId, storageId, _storageManager),
      _audioEngine(playbackId, Buffers::SPK_RX_BUF) {}

NexusPlayer::~NexusPlayer() {
    stop();
    if (_mutex != nullptr) {
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
    }
}

bool NexusPlayer::begin() {
    ESP_LOGI(TAG, "NexusPlayer initialization");
    
    _mutex = xSemaphoreCreateRecursiveMutex();
    if (!_mutex) {
        ESP_LOGE(TAG, "Failed to create recursive mutex");
        return false;
    }

    return _audioEngine.initialize(32000, 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Public controls
// ─────────────────────────────────────────────────────────────────────────────

void NexusPlayer::play(const char* songId, const char* downloadUrl) {
    PlayerLock lock(_mutex);

    if (!songId || !downloadUrl) {
        ESP_LOGE(TAG, "Invalid play arguments");
        return;
    }

    if (_session_active) {
        ESP_LOGI(TAG, "Play requested during active session. Deferring songId: %s until session ends.", songId);
        strncpy(_pendingSongId, songId, sizeof(_pendingSongId) - 1);
        _pendingSongId[sizeof(_pendingSongId) - 1] = '\0';
        strncpy(_pendingDownloadUrl, downloadUrl, sizeof(_pendingDownloadUrl) - 1);
        _pendingDownloadUrl[sizeof(_pendingDownloadUrl) - 1] = '\0';
        _should_play_after_session = true;
        _should_resume_after_session = false;
        return;
    }

    play_internal(songId, downloadUrl);
}

void NexusPlayer::play_internal(const char* songId, const char* downloadUrl) {
    ESP_LOGI(TAG, "Play requested for songId: %s, url: %s", songId, downloadUrl);

    // Starting a new song cancels any deferred session resumption/play
    _should_resume_after_session = false;
    _should_play_after_session = false;

    // Clear next-slot and prefetch state for fresh start
    clearNextSlot();
    _nextRequestSent = false;
    _waitingForPlayNextAsPlay = false;
    _downloadWasComplete = false;

    // If currently playing, stop it first
    if (_state != STATE_IDLE) {
        stopActivePipelines();
        _savedPcmLen = 0;
        _state = STATE_IDLE;
        _activeSongId[0] = '\0';
    }

    strncpy(_activeSongId, songId, sizeof(_activeSongId) - 1);
    _activeSongId[sizeof(_activeSongId) - 1] = '\0';

    // Flush all buffers before starting new session
    BufferManager::getInstance().flush(_playbackId);
    BufferManager::getInstance().flush(_storageId);
    BufferManager::getInstance().flush(Buffers::SPK_RX_BUF);
    if (BufferManager::getInstance().handle(Buffers::BT_SPK_BUF)) {
        BufferManager::getInstance().flush(Buffers::BT_SPK_BUF);
    }

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

        // Cache hit: download already done, immediately request next song
        _downloadWasComplete = true;
        requestNextSong();

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
        // _downloadWasComplete stays false; run() will poll and call onDownloadComplete()
    }
}

void NexusPlayer::pause() {
    PlayerLock lock(_mutex);
    _should_resume_after_session = false;
    _should_play_after_session = false;
    pause_internal();
}

void NexusPlayer::pause_internal() {
    if (_state == STATE_STREAMING_AND_CACHING || _state == STATE_LOCAL_PLAYBACK) {
        ESP_LOGI(TAG, "Pausing playback");
        _audioEngine.pause();

        // Give the audio engine a tiny delay to yield if it was actively running
        vTaskDelay(pdMS_TO_TICKS(5));

        // Pause the speaker playback task draining to stop instantly
        AudioPipelineManager::setSpeakerPaused(true);

        _state = STATE_PAUSED;
    }
}

void NexusPlayer::resume() {
    PlayerLock lock(_mutex);
    if (_session_active) {
        ESP_LOGI(TAG, "Resume requested during active session. Deferring until session ends.");
        _should_resume_after_session = true;
        _should_play_after_session = false;
    } else {
        resume_internal();
    }
}

void NexusPlayer::resume_internal() {
    if (_state == STATE_PAUSED) {
        ESP_LOGI(TAG, "Resuming playback");

        // Resume the speaker playback task draining
        AudioPipelineManager::setSpeakerPaused(false);

        _audioEngine.resume();
        if (_streamManager.isStreaming()) {
            _state = STATE_STREAMING_AND_CACHING;
        } else {
            _state = STATE_LOCAL_PLAYBACK;
        }
    }
}

void NexusPlayer::stop() {
    PlayerLock lock(_mutex);
    if (_state == STATE_IDLE) {
        return;
    }
    ESP_LOGI(TAG, "Stopping playback and active pipelines");
    stopActivePipelines();
    _state = STATE_IDLE;
    _activeSongId[0] = '\0';
    _should_resume_after_session = false;
    _should_play_after_session = false;
    _nextRequestSent = false;
    _waitingForPlayNextAsPlay = false;
    _downloadWasComplete = false;
}

void NexusPlayer::stopActivePipelines() {
    // 1. Stop streaming from network (kills HTTP connection and net task)
    _streamManager.stopStreaming();

    // 2. Unblock AudioEngine decoder task from waiting on PLAYER_BUF
    AudioChunkHeader eof_header = {ChunkType::EOF_STREAM, 0};
    BufferManager::getInstance().send(_playbackId, &eof_header, sizeof(eof_header));

    // Also send to storageId just in case writer is waiting
    BufferManager::getInstance().send(_storageId, &eof_header, sizeof(eof_header));

    // 3. Stop AudioEngine (kills decoder task)
    _audioEngine.stop();

    // 4. Stop and clean up SD Reader/Writer tasks and active file streams
    _storageManager.closeActiveFile();

    // Ensure the speaker playback task is not left paused
    AudioPipelineManager::setSpeakerPaused(false);

    // 5. Flush all buffers
    BufferManager::getInstance().flush(_playbackId);
    BufferManager::getInstance().flush(_storageId);
    BufferManager::getInstance().flush(Buffers::SPK_RX_BUF);
    if (BufferManager::getInstance().handle(Buffers::BT_SPK_BUF)) {
        BufferManager::getInstance().flush(Buffers::BT_SPK_BUF);
    }
}

void NexusPlayer::playAlert(AlertType type) {
    PlayerLock lock(_mutex);
    _audioEngine.playAlert(type);
}

// ─────────────────────────────────────────────────────────────────────────────
// Prefetch / Next-slot management
// ─────────────────────────────────────────────────────────────────────────────

void NexusPlayer::requestNextSong() {
    if (!_nextRequestSent) {
        _nextRequestSent = true;
        _prefetchState = PrefetchState::WAITING;
        ESP_LOGI(TAG, "Requesting next song from server");
        MqttService::getInstance().publish("mpv/command", "{\"cmd\":\"next\"}");
    } else {
        ESP_LOGI(TAG, "Next request already in flight, skipping duplicate publish");
    }
}

void NexusPlayer::onDownloadComplete() {
    // Called once per song when its HTTP download finishes successfully.
    // At this point: writer task has exited, _storageId buffer is idle, reader is still playing.
    ESP_LOGI(TAG, "Download complete for songId: %s", _activeSongId);
    _downloadWasComplete = true;

    // If a prefetch song was already queued but waiting for the main download to finish, start it now.
    if (_nextSongId[0] != '\0') {
        if (_prefetchState == PrefetchState::WAITING) {
            ESP_LOGI(TAG, "Starting deferred prefetch for next song: %s", _nextSongId);
            _prefetchState = PrefetchState::DOWNLOADING;
            _storageManager.beginPrefetch(_nextSongId);
            if (!_streamManager.beginStreaming(_nextSongUrl)) {
                ESP_LOGE(TAG, "Failed to start prefetch stream for: %s", _nextSongId);
                _prefetchState = PrefetchState::NONE;
                _nextSongId[0] = '\0';
                _nextSongUrl[0] = '\0';
            }
        } else {
            ESP_LOGI(TAG, "Next song %s already queued in state %d", _nextSongId, (int)_prefetchState);
        }
    } else {
        requestNextSong();
    }
}

void NexusPlayer::setNextSong(const char* songId, const char* url) {
    if (!songId || !url || songId[0] == '\0') {
        ESP_LOGW(TAG, "setNextSong: invalid arguments");
        return;
    }

    PlayerLock lock(_mutex);

    // Acknowledge the server response — clear the in-flight flag
    _nextRequestSent = false;

    // Edge case: skip was pressed while next slot was empty, song was stopped.
    // Route this response directly to play_internal instead of the prefetch slot.
    if (_waitingForPlayNextAsPlay) {
        ESP_LOGI(TAG, "setNextSong: waitingForPlayNextAsPlay — routing to play_internal: %s", songId);
        _waitingForPlayNextAsPlay = false;
        play_internal(songId, url);
        return;
    }

    // Normal case: store in next slot and start prefetch
    strncpy(_nextSongId, songId, sizeof(_nextSongId) - 1);
    _nextSongId[sizeof(_nextSongId) - 1] = '\0';
    strncpy(_nextSongUrl, url, sizeof(_nextSongUrl) - 1);
    _nextSongUrl[sizeof(_nextSongUrl) - 1] = '\0';

    if (_storageManager.fileExists(songId)) {
        _prefetchState = PrefetchState::CACHED;
        ESP_LOGI(TAG, "Next song %s already cached — no prefetch needed", songId);
    } else {
        // If current song is still streaming and caching, we MUST NOT start the prefetch stream yet.
        // Doing so would interrupt the active download and cause a crash.
        // Instead, mark the prefetch as WAITING. It will be started in onDownloadComplete().
        if (!_downloadWasComplete) {
            ESP_LOGI(TAG, "Current song download is still active. Deferring prefetch of: %s", songId);
            _prefetchState = PrefetchState::WAITING;
        } else {
            // Prefetch: reuse StreamManager + StorageManager in writer-only mode.
            // Safe because main download is already done (onDownloadComplete fired before this).
            ESP_LOGI(TAG, "Prefetching next song: %s", songId);
            _prefetchState = PrefetchState::DOWNLOADING;
            _storageManager.beginPrefetch(songId);
            if (!_streamManager.beginStreaming(url)) {
                ESP_LOGE(TAG, "Failed to start prefetch stream for: %s", songId);
                _prefetchState = PrefetchState::NONE;
                _nextSongId[0] = '\0';
                _nextSongUrl[0] = '\0';
            }
        }
    }
}

void NexusPlayer::playNext() {
    PlayerLock lock(_mutex);
    ESP_LOGI(TAG, "playNext() called — prefetchState=%d, nextSongId='%s'",
             (int)_prefetchState, _nextSongId);

    if (_nextSongId[0] != '\0') {
        // Next slot has a song queued (cached or still downloading)
        // Stop the background prefetch; play_internal will handle via cache-hit or stream path
        _storageManager.stopPrefetch();
        _streamManager.stopStreaming();
        promoteNextToCurrent();
    } else {
        // Next slot is empty.
        // The server may or may not have a request in-flight already.
        // Either way, set the flag so the next play_next response goes to current slot.
        ESP_LOGI(TAG, "playNext(): next slot empty — stopping current, waiting for server response");
        _waitingForPlayNextAsPlay = true;

        // Stop current playback
        stopActivePipelines();
        _state = STATE_IDLE;
        _activeSongId[0] = '\0';
        _downloadWasComplete = false;

        // Request next if not already in flight
        // (server deduplicates on its side too)
        requestNextSong();
    }
}

void NexusPlayer::promoteNextToCurrent() {
    // Capture next slot before clearing
    char songId[64];
    char url[256];
    strncpy(songId, _nextSongId, sizeof(songId));
    strncpy(url, _nextSongUrl, sizeof(url));

    // Clear next slot
    clearNextSlot();
    _nextRequestSent = false;
    _downloadWasComplete = false;

    // Stop current pipelines (does nothing if already idle)
    if (_state != STATE_IDLE) {
        stopActivePipelines();
        _state = STATE_IDLE;
        _activeSongId[0] = '\0';
    }

    ESP_LOGI(TAG, "Promoting next song to current: %s", songId);
    play_internal(songId, url);
}

void NexusPlayer::clearNextSlot() {
    // Stop any in-progress prefetch
    _storageManager.stopPrefetch();

    _nextSongId[0]  = '\0';
    _nextSongUrl[0] = '\0';
    _prefetchState  = PrefetchState::NONE;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReactorTask
// ─────────────────────────────────────────────────────────────────────────────

void NexusPlayer::onStateChanged(ComponentMask changed, const SystemState& snap) {
    if (changed & COMP::ASSISTANT) {
        PlayerLock lock(_mutex);
        
        bool new_session_active = (snap.assistant.session_state != AssistantState::Idle);
        
        if (new_session_active && !_session_active) {
            ESP_LOGI(TAG, "Assistant session became active. Interrupting NexusPlayer if playing.");
            _session_active = true;
            if (_state == STATE_STREAMING_AND_CACHING || _state == STATE_LOCAL_PLAYBACK) {
                _should_resume_after_session = true;
                _should_play_after_session = false;
                pause_internal();

                // Flush SPK_RX_BUF to immediately discard any buffered music
                BufferManager::getInstance().flush(Buffers::SPK_RX_BUF);
            } else {
                _should_resume_after_session = false;
            }
            // Ensure speaker is unpaused during assistant session to play alerts & assistant voice
            AudioPipelineManager::setSpeakerPaused(false);
        } 
        else if (!new_session_active && _session_active) {
            ESP_LOGI(TAG, "Assistant session ended. Handling deferred playback actions.");
            _session_active = false;
            if (_should_play_after_session) {
                play_internal(_pendingSongId, _pendingDownloadUrl);
                _should_play_after_session = false;
            } else if (_should_resume_after_session) {
                resume_internal();
            } else {
                // If we were paused before the session, restore the speaker pause state
                if (_state == STATE_PAUSED) {
                    AudioPipelineManager::setSpeakerPaused(true);
                }
            }
            _should_resume_after_session = false;
        }
    }
}

void NexusPlayer::run() {
    ESP_LOGI(TAG, "NexusPlayer background task started");
    while (m_running) {
        uint32_t changed_bits = 0;
        BaseType_t notified = xTaskNotifyWait(0, 0xFFFFFFFF, &changed_bits, pdMS_TO_TICKS(100));
        if (!m_running) break;

        if (notified == pdTRUE && changed_bits > 0) {
            m_last_changed = changed_bits;
            SystemState snap = EmbeddedSysDb::getInstance().snapshot();
            onStateChanged(m_last_changed, snap);
        }

        // Check if connection alerts are pending (set by GeminiProtocol)
        SystemState snap = EmbeddedSysDb::getInstance().snapshot();
        if (snap.assistant.play_connection_alerts) {
            ESP_LOGI(TAG, "Playing connection alerts on NexusPlayer task...");
            AudioPipelineManager::suspendDrainer();
            playAlert(ALERT_WAKE_CONFIRM);
            AudioPipelineManager::resumeDrainer();

            // Clear flag and advance state to start mic pumping
            EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
                s.assistant.play_connection_alerts = false;
                s.assistant.session_state = AssistantState::StreamingUserAudio;
                s.assistant.visual_state  = AssistantVisualState::Listening;
                s.pipeline.mode           = PipelineMode::GEMINI_LIVE;
                s.audio.session_active    = true;
                s.audio.mic_enabled       = true;
            });
        }

        // Check if natural close alerts are pending (set by AudioService/AssistantService)
        if (snap.assistant.session_state == AssistantState::Closing && snap.assistant.close_is_natural) {
            ESP_LOGI(TAG, "Playing session end alert on NexusPlayer task...");
            AudioPipelineManager::suspendDrainer();
            playAlert(ALERT_SESSION_END);
            AudioPipelineManager::resumeDrainer();

            EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
                s.assistant.close_is_natural = false;
                // Transition to Idle to trigger AssistantService and AudioService cleanup
                s.assistant.session_state = AssistantState::Idle;
            });
        }

        // Poll for main download completion to trigger next-song prefetch request
        {
            PlayerLock lock(_mutex);
            if (!_downloadWasComplete &&
                (_state == STATE_STREAMING_AND_CACHING || _state == STATE_LOCAL_PLAYBACK) &&
                _streamManager.isDownloadComplete()) {
                onDownloadComplete();
            }

            // Poll for prefetch download completion to update prefetch state
            if (_prefetchState == PrefetchState::DOWNLOADING &&
                _storageManager.isPrefetchComplete()) {
                ESP_LOGI(TAG, "Prefetch download complete for: %s", _nextSongId);
                _prefetchState = PrefetchState::CACHED;
            }
        }

        checkPlaybackFinished();
    }
}

void NexusPlayer::checkPlaybackFinished() {
    PlayerLock lock(_mutex);
    if (_state == STATE_STREAMING_AND_CACHING || _state == STATE_LOCAL_PLAYBACK) {
        if (!_audioEngine.isPlaying()) {
            auto &bm = BufferManager::getInstance();
            if (bm.getUsedBytes(Buffers::SPK_RX_BUF) == 0) {
                ESP_LOGI(TAG, "Playback naturally finished for songId: %s", _activeSongId);

                if (_nextSongId[0] != '\0') {
                    // Next song is queued (cached or still being prefetched)
                    // promoteNextToCurrent handles both cases via play_internal
                    ESP_LOGI(TAG, "Next song ready — promoting: %s", _nextSongId);
                    promoteNextToCurrent();
                } else {
                    // No next song queued yet — go idle and wait.
                    // requestNextSong() was already called in onDownloadComplete(),
                    // so the server response (setNextSong) will arrive and trigger
                    // _waitingForPlayNextAsPlay logic or a direct play_internal call.
                    ESP_LOGI(TAG, "No next song queued — going idle. Server response pending.");
                    stopActivePipelines();
                    _state = STATE_IDLE;
                    _activeSongId[0] = '\0';
                    // Mark that we want the next play_next response as current song
                    _waitingForPlayNextAsPlay = true;
                }
            }
        }
    }
}
