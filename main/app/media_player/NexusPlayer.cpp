#include "NexusPlayer.h"
#include "esp_log.h"
#include <cstring>
#include <algorithm>
#include "app/audio/SpeakerPlayback.h"
#include "app/audio/AudioOrchestrator.h"
#include "common/thread_config.h"

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
          ThreadConfig::StackSize::STACK_PLAYER,
          ThreadConfig::Priority::GEMINI_PROTOCOL,
          ThreadConfig::CORE_NETWORK,
          COMP::ASSISTANT
      }),
      _playbackId(playbackId),
      _storageId(storageId),
      _storageManager(playbackId, storageId),
      _streamManager(playbackId, storageId),
      _audioEngine(playbackId, Buffers::MEDIA_RX_BUF) {}

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

    AudioOrchestrator::getInstance().addObserver(this);
    return _audioEngine.initialize(32000, 1);
}

void NexusPlayer::addObserver(IPlaybackObserver* observer) {
    PlayerLock lock(_mutex);
    if (observer) {
        _observers.push_back(observer);
    }
}

void NexusPlayer::removeObserver(IPlaybackObserver* observer) {
    PlayerLock lock(_mutex);
    _observers.erase(std::remove(_observers.begin(), _observers.end(), observer), _observers.end());
}

void NexusPlayer::notifyTrackStarted(const char* songId) {
    std::vector<IPlaybackObserver*> obsCopy;
    {
        PlayerLock lock(_mutex);
        obsCopy = _observers;
    }
    for (auto* obs : obsCopy) {
        if (obs) obs->onTrackStarted(songId);
    }
}

void NexusPlayer::notifyTrackFinished(const char* songId) {
    std::vector<IPlaybackObserver*> obsCopy;
    {
        PlayerLock lock(_mutex);
        obsCopy = _observers;
    }
    for (auto* obs : obsCopy) {
        if (obs) obs->onTrackFinished(songId);
    }
}

void NexusPlayer::notifyPlaybackError(const char* songId, int err) {
    std::vector<IPlaybackObserver*> obsCopy;
    {
        PlayerLock lock(_mutex);
        obsCopy = _observers;
    }
    for (auto* obs : obsCopy) {
        if (obs) obs->onPlaybackError(songId, err);
    }
}

void NexusPlayer::onAudioFocusChange(AudioTrack track, FocusEvent event) {
    if (track == AudioTrack::MEDIA) {
        PlayerLock lock(_mutex);
        if (event == FocusEvent::LOSS_PAUSE) {
            if (_state == STATE_STREAMING_AND_CACHING || _state == STATE_LOCAL_PLAYBACK) {
                ESP_LOGI(TAG, "Audio focus lost (pause) — pausing media playback");
                _should_resume_after_session = true;
                pause_internal();
            }
        } else if (event == FocusEvent::GAIN) {
            if (_state == STATE_PAUSED && _should_resume_after_session) {
                ESP_LOGI(TAG, "Audio focus gained (resume) — resuming media playback");
                _should_resume_after_session = false;
                resume_internal();
            }
        }
    }
}

void NexusPlayer::play(const char* songId, const char* downloadUrl) {
    PlayerLock lock(_mutex);

    if (!songId || !downloadUrl) {
        ESP_LOGE(TAG, "Invalid play arguments");
        return;
    }

    if (_session_active) {
        auto snap = EmbeddedSysDb::getInstance().snapshot();
        if (snap.assistant.session_state == AssistantState::WaitingForFollowup) {
            ESP_LOGI(TAG, "Play requested during WaitingForFollowup. Terminating assistant session immediately to start playback.");
            _session_active = false;
            EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
                s.assistant.session_state = AssistantState::Idle;
                s.assistant.media_pending_idle = false;
            });
        } else {
            ESP_LOGI(TAG, "Play requested during active session. Deferring songId: %s until session ends.", songId);
            _pendingSongId = songId;
            _pendingDownloadUrl = downloadUrl;
            _should_play_after_session = true;
            _should_resume_after_session = false;
            return;
        }
    }

    play_internal(songId, downloadUrl);
}

void NexusPlayer::play_internal(const char* songId, const char* downloadUrl) {
    ESP_LOGI(TAG, "Play requested for songId: %s, url: %s", songId, downloadUrl);

    _should_resume_after_session = false;
    _should_play_after_session = false;

    if (_state != STATE_IDLE) {
        stop();
    }

    strncpy(_activeSongId, songId, sizeof(_activeSongId) - 1);
    _activeSongId[sizeof(_activeSongId) - 1] = '\0';

    // Flush buffers before starting new stream
    BufferManager::getInstance().flush(_playbackId);
    BufferManager::getInstance().flush(_storageId);
    BufferManager::getInstance().flush(Buffers::MEDIA_RX_BUF);

    notifyTrackStarted(songId);
    AudioOrchestrator::getInstance().notifyMediaStarted();

    if (_storageManager.fileExists(songId)) {
        ESP_LOGI(TAG, "Cache Hit! Playing local file for songId: %s", songId);
        _state = STATE_LOCAL_PLAYBACK;
        _audioEngine.start();

        if (!_storageManager.openFileForReading(songId)) {
            ESP_LOGE(TAG, "Failed to open local file for reading");
            stopActivePipelines();
            _state = STATE_IDLE;
            notifyPlaybackError(songId, -1);
            return;
        }
    } else {
        auto snap = EmbeddedSysDb::getInstance().snapshot();
        bool doCache = snap.audio.cache_downloads;

        if (doCache) {
            ESP_LOGI(TAG, "Cache Miss! Downloading and streaming with caching songId: %s", songId);
            _state = STATE_STREAMING_AND_CACHING;
            _audioEngine.start();

            if (!_storageManager.openFileForCaching(songId)) {
                ESP_LOGE(TAG, "Failed to open file for caching");
                stopActivePipelines();
                _state = STATE_IDLE;
                notifyPlaybackError(songId, -2);
                return;
            }

            if (!_streamManager.beginStreaming(downloadUrl, true)) {
                ESP_LOGE(TAG, "Failed to start streaming");
                stopActivePipelines();
                _state = STATE_IDLE;
                notifyPlaybackError(songId, -3);
                return;
            }
        } else {
            ESP_LOGI(TAG, "Cache Miss! Pure live streaming (no SD cache) songId: %s", songId);
            _state = STATE_STREAMING_AND_CACHING;
            _audioEngine.start();

            if (!_streamManager.beginStreaming(downloadUrl, false)) {
                ESP_LOGE(TAG, "Failed to start live streaming");
                stopActivePipelines();
                _state = STATE_IDLE;
                notifyPlaybackError(songId, -3);
                return;
            }
        }
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
        _state = STATE_PAUSED;
        AudioOrchestrator::getInstance().notifyMediaStopped();
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
        _audioEngine.resume();
        if (_streamManager.isStreaming()) {
            _state = STATE_STREAMING_AND_CACHING;
        } else {
            _state = STATE_LOCAL_PLAYBACK;
        }
        AudioOrchestrator::getInstance().notifyMediaStarted();
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
    _pendingSongId.clear();
    _pendingDownloadUrl.clear();
    AudioOrchestrator::getInstance().notifyMediaStopped();
}

void NexusPlayer::stopActivePipelines() {
    // 1. Stop streaming from network
    _streamManager.stopStreaming();

    // 2. Unblock AudioEngine decoder task from waiting on PLAYER_BUF
    AudioChunkHeader eof_header = {ChunkType::EOF_STREAM, 0};
    BufferManager::getInstance().send(_playbackId, &eof_header, sizeof(eof_header));
    BufferManager::getInstance().send(_storageId, &eof_header, sizeof(eof_header));

    // 3. Stop AudioEngine
    _audioEngine.stop();

    // 4. Stop and clean up SD Reader/Writer tasks and active file streams
    _storageManager.closeActiveFile();

    // 5. Flush all buffers
    BufferManager::getInstance().flush(_playbackId);
    BufferManager::getInstance().flush(_storageId);
    BufferManager::getInstance().flush(Buffers::MEDIA_RX_BUF);
}

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
            } else {
                _should_resume_after_session = false;
            }
        } 
        else if (!new_session_active && _session_active) {
            ESP_LOGI(TAG, "Assistant session ended. Handling deferred playback actions.");
            _session_active = false;
            if (_should_play_after_session && (!_pendingDownloadUrl.empty() || _storageManager.fileExists(_pendingSongId.c_str()))) {
                play_internal(_pendingSongId.c_str(), _pendingDownloadUrl.c_str());
                _pendingSongId.clear();
                _pendingDownloadUrl.clear();
                _should_play_after_session = false;
            } else if (_should_resume_after_session) {
                resume_internal();
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

        checkPlaybackFinished();
    }
}

void NexusPlayer::checkPlaybackFinished() {
    char finishedSong[64] = {0};
    bool trackFinished = false;

    {
        PlayerLock lock(_mutex);
        if (_state == STATE_STREAMING_AND_CACHING || _state == STATE_LOCAL_PLAYBACK) {
            if (!_audioEngine.isPlaying()) {
                auto &bm = BufferManager::getInstance();
                if (bm.getUsedBytes(Buffers::MEDIA_RX_BUF) == 0) {
                    ESP_LOGI(TAG, "Playback naturally finished for songId: %s. Notifying observers.", _activeSongId);
                    strncpy(finishedSong, _activeSongId, sizeof(finishedSong) - 1);
                    finishedSong[sizeof(finishedSong) - 1] = '\0';
                    
                    stopActivePipelines();
                    _state = STATE_IDLE;
                    _activeSongId[0] = '\0';
                    _should_resume_after_session = false;
                    _should_play_after_session = false;
                    _pendingSongId.clear();
                    _pendingDownloadUrl.clear();
                    AudioOrchestrator::getInstance().notifyMediaStopped();
                    trackFinished = true;
                }
            }
        }
    }

    if (trackFinished) {
        // Notify observers (MusicPlaybackService) outside the player lock
        notifyTrackFinished(finishedSong);
    }
}
