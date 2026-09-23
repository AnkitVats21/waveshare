#include "core_sysdb/AudioRates.h"
#include "NexusPlayer.h"
#include "media_player/CatalogDB.h"
#include "media_player/MusicPlaybackService.h"
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
          COMP::ASSISTANT | COMP::BLUETOOTH
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
    _audioEngine.setSeekIndexCallback([this](uint32_t timecodeMs, uint32_t byteOffset) {
        PlayerLock lock(_mutex);
        if (_sessionSeekTable.size() < 100) {
            _sessionSeekTable.push_back({timecodeMs, byteOffset});
        }
    });
    return _audioEngine.initialize(MIXER_SAMPLE_RATE, 1);
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
            // During an assistant session the resume waits for the session to end
            // (onStateChanged), so a mid-session focus gain doesn't restart music.
            if (_state == STATE_PAUSED && _should_resume_after_session && !_session_active) {
                ESP_LOGI(TAG, "Audio focus gained (resume) — resuming media playback");
                _should_resume_after_session = false;
                resume_internal();
            }
        }
    }
}

void NexusPlayer::play(const char* songId, const char* downloadUrl) {
    playAt(songId, downloadUrl, 0);
}

void NexusPlayer::playAt(const char* songId, const char* downloadUrl, uint32_t startPosMs) {
    PlayerLock lock(_mutex);

    if (!songId || (!downloadUrl && !_storageManager.fileExists(songId))) {
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
            _pendingDownloadUrl = downloadUrl ? downloadUrl : "";
            _pendingStartPosMs = startPosMs;
            _should_play_after_session = true;
            _should_resume_after_session = false;
            return;
        }
    }

    play_internal(songId, downloadUrl, startPosMs);
}

void NexusPlayer::play_internal(const char* songId, const char* downloadUrl, uint32_t startPosMs) {
    ESP_LOGI(TAG, "Play requested for songId: %s, url: %s, startPos: %u ms",
             songId, downloadUrl ? downloadUrl : "(local)", (unsigned int)startPosMs);

    _should_resume_after_session = false;
    _should_play_after_session = false;
    _pendingStartPosMs = 0;

    if (_state != STATE_IDLE) {
        stop();
    }

    strncpy(_activeSongId, songId, sizeof(_activeSongId) - 1);
    _activeSongId[sizeof(_activeSongId) - 1] = '\0';
    _activeDownloadUrl = downloadUrl ? downloadUrl : "";
    _sessionSeekTable.clear();

    // Flush buffers before starting new stream
    BufferManager::getInstance().flush(_playbackId);
    BufferManager::getInstance().flush(_storageId);
    BufferManager::getInstance().flush(Buffers::MEDIA_RX_BUF);

    notifyTrackStarted(songId);
    AudioOrchestrator::getInstance().notifyMediaStarted();

    if (_storageManager.fileExists(songId)) {
        ESP_LOGI(TAG, "Cache Hit! Playing local file for songId: %s", songId);
        _state = STATE_LOCAL_PLAYBACK;

        if (!_storageManager.openFileForReading(songId)) {
            ESP_LOGE(TAG, "Failed to open local file for reading");
            stopActivePipelines();
            _state = STATE_IDLE;
            EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
                s.media.state = MediaPlaybackState::ERROR_STATE;
                s.media.active_song_id[0] = '\0';
            });
            notifyPlaybackError(songId, -1);
            return;
        }

        if (startPosMs > 0) {
            uint32_t nearestTime = 0;
            uint32_t nearestOffset = 0;
            bool hasOffset = CatalogDB::getInstance().lookupSeekEntry(songId, startPosMs, nearestTime, nearestOffset);
            if (!hasOffset) {
                nearestOffset = (startPosMs / 1000) * 16000;
            }
            _audioEngine.resetDecoder();
            _audioEngine.setStreamByteOffset(nearestOffset);
            _storageManager.seekTo(nearestOffset);
            BufferManager::getInstance().flush(Buffers::MEDIA_RX_BUF);
        }

        _audioEngine.start();
        EmbeddedSysDb::getInstance().mutate([songId, startPosMs](SystemState& s) {
            s.media.state = MediaPlaybackState::PLAYING;
            s.media.output_target = MediaOutputTarget::LOCAL;
            strncpy(s.media.active_song_id, songId, sizeof(s.media.active_song_id) - 1);
            s.media.active_song_id[sizeof(s.media.active_song_id) - 1] = '\0';
            s.media.position_ms = startPosMs;
        });
    } else {
        auto snap = EmbeddedSysDb::getInstance().snapshot();
        bool doCache = snap.media.cache_downloads && (startPosMs == 0);

        _state = STATE_STREAMING_AND_CACHING;

        if (doCache) {
            ESP_LOGI(TAG, "Cache Miss! Downloading and streaming with caching songId: %s", songId);
            if (!_storageManager.openFileForCaching(songId)) {
                ESP_LOGE(TAG, "Failed to open file for caching");
                stopActivePipelines();
                _state = STATE_IDLE;
                EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
                    s.media.state = MediaPlaybackState::ERROR_STATE;
                    s.media.active_song_id[0] = '\0';
                });
                notifyPlaybackError(songId, -2);
                return;
            }

            _audioEngine.start();
            EmbeddedSysDb::getInstance().mutate([songId](SystemState& s) {
                s.media.state = MediaPlaybackState::PLAYING;
                s.media.output_target = MediaOutputTarget::LOCAL;
                strncpy(s.media.active_song_id, songId, sizeof(s.media.active_song_id) - 1);
                s.media.active_song_id[sizeof(s.media.active_song_id) - 1] = '\0';
                s.media.position_ms = 0;
            });

            if (!_streamManager.beginStreaming(downloadUrl, true)) {
                ESP_LOGE(TAG, "Failed to start streaming");
                stopActivePipelines();
                _state = STATE_IDLE;
                EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
                    s.media.state = MediaPlaybackState::ERROR_STATE;
                    s.media.active_song_id[0] = '\0';
                });
                notifyPlaybackError(songId, -3);
                return;
            }
        } else {
            ESP_LOGI(TAG, "Streaming songId: %s (startPos: %u ms)", songId, (unsigned int)startPosMs);
            _audioEngine.start();
            EmbeddedSysDb::getInstance().mutate([songId, startPosMs](SystemState& s) {
                s.media.state = MediaPlaybackState::PLAYING;
                s.media.output_target = MediaOutputTarget::LOCAL;
                strncpy(s.media.active_song_id, songId, sizeof(s.media.active_song_id) - 1);
                s.media.active_song_id[sizeof(s.media.active_song_id) - 1] = '\0';
                s.media.position_ms = startPosMs;
            });

            bool streamOk = false;
            if (startPosMs > 0) {
                uint32_t nearestTime = 0;
                uint32_t nearestOffset = 0;
                bool hasOffset = CatalogDB::getInstance().lookupSeekEntry(songId, startPosMs, nearestTime, nearestOffset);
                if (!hasOffset) {
                    nearestOffset = (startPosMs / 1000) * 16000;
                }
                _audioEngine.resetDecoder();
                _audioEngine.setStreamByteOffset(nearestOffset);
                streamOk = _streamManager.beginStreamingFrom(downloadUrl, nearestOffset, false);
            } else {
                streamOk = _streamManager.beginStreaming(downloadUrl, false);
            }

            if (!streamOk) {
                ESP_LOGE(TAG, "Failed to start streaming");
                stopActivePipelines();
                _state = STATE_IDLE;
                EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
                    s.media.state = MediaPlaybackState::ERROR_STATE;
                    s.media.active_song_id[0] = '\0';
                });
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
        EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
            s.media.state = MediaPlaybackState::PAUSED;
        });
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
        EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
            s.media.state = MediaPlaybackState::PLAYING;
        });
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
    EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
        s.media.state = MediaPlaybackState::IDLE;
        s.media.active_song_id[0] = '\0';
    });
}

void NexusPlayer::stopActivePipelines() {
    commitSessionSeekTable();
    auto& bm = BufferManager::getInstance();

    // 1. Signal the decoder to stop, then clear the rings it feeds on/into so it
    //    can't be wedged by output backpressure (its throttle) or a full input.
    _audioEngine.stop();
    bm.flush(Buffers::MEDIA_RX_BUF);   // release decoder's PCM-output backpressure
    bm.flush(_playbackId);             // discard pending compressed audio, make room
    bm.flush(_storageId);

    // 2. Wake a decoder that's parked on an empty input ring so it sees the stop
    //    flag and exits (flush alone does not unblock a blocked reader).
    AudioChunkHeader eof_header = {ChunkType::EOF_STREAM, 0};
    bm.send(_playbackId, &eof_header, sizeof(eof_header));
    bm.send(_storageId, &eof_header, sizeof(eof_header));
    if (!_audioEngine.waitUntilStopped()) {
        ESP_LOGW(TAG, "Decode task did not stop within timeout");
    }

    // 3. Network task can now finish its bounded EOF write into an empty ring and exit.
    _streamManager.stopStreaming();

    // 4. Stop and clean up SD Reader/Writer tasks and active file streams
    _storageManager.closeActiveFile();

    // 5. Final flush - clear the EOF markers and anything the network task left.
    bm.flush(_playbackId);
    bm.flush(_storageId);
    bm.flush(Buffers::MEDIA_RX_BUF);
}

void NexusPlayer::onStateChanged(ComponentMask changed, const SystemState& snap) {
    if (changed & COMP::ASSISTANT) {
        PlayerLock lock(_mutex);
        
        bool new_session_active = (snap.assistant.session_state != AssistantState::Idle);
        
        if (new_session_active && !_session_active) {
            _session_active = true;
        } 
        else if (!new_session_active && _session_active) {
            ESP_LOGI(TAG, "Assistant session ended. Handling deferred playback actions.");
            _session_active = false;
            if (_should_play_after_session && (!_pendingDownloadUrl.empty() || _storageManager.fileExists(_pendingSongId.c_str()))) {
                play_internal(_pendingSongId.c_str(), _pendingDownloadUrl.c_str(), _pendingStartPosMs);
                _pendingSongId.clear();
                _pendingDownloadUrl.clear();
                _pendingStartPosMs = 0;
                _should_play_after_session = false;
            } else if (_should_resume_after_session && _state == STATE_PAUSED) {
                // Set by the wake-word pause (onAudioFocusChange) or by a "resume"
                // request made during the session (resume()). A "pause"/"stop"
                // request during the session clears it, so the music stays paused.
                ESP_LOGI(TAG, "Resuming playback paused for the assistant session");
                _should_resume_after_session = false;
                resume_internal();
            }
        }
    }

    if ((changed & COMP::BLUETOOTH) && (changed & BIT_BLUETOOTH::CONNECTED)) {
        MusicPlaybackService::getInstance().onBluetoothConnectionChanged(snap.bluetooth.connected);
    }
}

void NexusPlayer::run() {
    ESP_LOGI(TAG, "NexusPlayer background task started");
    uint32_t tick_count = 0;
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

        // Periodically update position_ms in SysDb (~every 1 second) during local playback
        if (++tick_count >= 10) {
            tick_count = 0;
            if (_state == STATE_STREAMING_AND_CACHING || _state == STATE_LOCAL_PLAYBACK) {
                uint32_t currentPos = _audioEngine.getPositionMs();
                EmbeddedSysDb::getInstance().mutate([currentPos](SystemState& s) {
                    if (s.media.output_target == MediaOutputTarget::LOCAL) {
                        s.media.position_ms = currentPos;
                    }
                });
            }
        }
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
                    EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
                        s.media.state = MediaPlaybackState::IDLE;
                        s.media.active_song_id[0] = '\0';
                    });
                }
            }
        }
    }

    if (trackFinished) {
        // Notify observers (MusicPlaybackService) outside the player lock
        notifyTrackFinished(finishedSong);
    }
}

void NexusPlayer::commitSessionSeekTable() {
    if (_activeSongId[0] != '\0' && !_sessionSeekTable.empty()) {
        CatalogDB::getInstance().setSeekTable(_activeSongId, _sessionSeekTable.data(), _sessionSeekTable.size());
        _sessionSeekTable.clear();
    }
}

uint32_t NexusPlayer::getPositionMs() const {
    return _audioEngine.getPositionMs();
}

void NexusPlayer::seekTo(uint32_t positionMs) {
    PlayerLock lock(_mutex);
    if (_state == STATE_IDLE || _activeSongId[0] == '\0') {
        ESP_LOGW(TAG, "Cannot seek: player is idle");
        return;
    }

    uint32_t nearestTime = 0;
    uint32_t nearestOffset = 0;
    bool hasOffset = CatalogDB::getInstance().lookupSeekEntry(_activeSongId, positionMs, nearestTime, nearestOffset);

    ESP_LOGI(TAG, "Seek to %u ms (nearest: time=%u ms, offset=%u, resolved=%d)",
             (unsigned int)positionMs, (unsigned int)nearestTime, (unsigned int)nearestOffset, (int)hasOffset);

    if (_state == STATE_LOCAL_PLAYBACK) {
        _audioEngine.resetDecoder();
        _audioEngine.setStreamByteOffset(nearestOffset);
        _storageManager.seekTo(nearestOffset);
        BufferManager::getInstance().flush(Buffers::MEDIA_RX_BUF);
    } else if (_state == STATE_STREAMING_AND_CACHING) {
        if (_activeDownloadUrl.empty()) {
            ESP_LOGW(TAG, "Cannot seek online stream: stream URL empty");
            return;
        }
        _streamManager.stopStreaming();
        _audioEngine.resetDecoder();
        _audioEngine.setStreamByteOffset(nearestOffset);
        BufferManager::getInstance().flush(_playbackId);
        BufferManager::getInstance().flush(Buffers::MEDIA_RX_BUF);

        _streamManager.beginStreamingFrom(_activeDownloadUrl.c_str(), nearestOffset, false);
    } else if (_state == STATE_PAUSED) {
        if (_storageManager.fileExists(_activeSongId)) {
            _audioEngine.resetDecoder();
            _audioEngine.setStreamByteOffset(nearestOffset);
            _storageManager.seekTo(nearestOffset);
            BufferManager::getInstance().flush(Buffers::MEDIA_RX_BUF);
        } else if (!_activeDownloadUrl.empty()) {
            _streamManager.stopStreaming();
            _audioEngine.resetDecoder();
            _audioEngine.setStreamByteOffset(nearestOffset);
            BufferManager::getInstance().flush(_playbackId);
            BufferManager::getInstance().flush(Buffers::MEDIA_RX_BUF);
            _streamManager.beginStreamingFrom(_activeDownloadUrl.c_str(), nearestOffset, false);
            _audioEngine.pause();
        }
    }
}

