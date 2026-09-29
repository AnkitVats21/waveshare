#include "core_sysdb/AudioRates.h"
#include "NexusPlayer.h"
#include "media_player/CatalogDB.h"
#include "media_player/MusicPlaybackService.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <cstring>
#include <memory>
#include <algorithm>
#include <cstdlib>
#include "app/audio/SpeakerPlayback.h"
#include "app/audio/AudioOrchestrator.h"
#include "common/thread_config.h"

namespace {
// The EBML magic that starts every WebM file: enough for the decoder factory
// to pick the WebM decoder when a stream starts mid-file.
constexpr uint8_t EBML_MAGIC[] = {0x1A, 0x45, 0xDF, 0xA3};
bool isWebmUrl(const char* url) { return url && strstr(url, "mime=audio%2Fwebm"); }
} // namespace

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

std::vector<IPlaybackObserver*> NexusPlayer::currentObservers() {
    PlayerLock lock(_mutex);
    if (_alarmOwner) return {_alarmObserver};
    return _observers;
}

void NexusPlayer::notifyTrackStarted(const char* songId) {
    for (auto* obs : currentObservers()) {
        if (obs) obs->onTrackStarted(songId);
    }
}

void NexusPlayer::notifyTrackFinished(const char* songId) {
    for (auto* obs : currentObservers()) {
        if (obs) obs->onTrackFinished(songId);
    }
}

void NexusPlayer::notifyPlaybackError(const char* songId, int err) {
    for (auto* obs : currentObservers()) {
        if (obs) obs->onPlaybackError(songId, err);
    }
}

void NexusPlayer::onAudioFocusChange(AudioTrack track, FocusEvent event) {
    if (track == AudioTrack::MEDIA && !_alarmOwner) {
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

    if (_alarmOwner) {
        ESP_LOGW(TAG, "Play of %s ignored: an alarm owns the player", songId ? songId : "?");
        return;
    }

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
    // Up to the query only: a stream URL's query is its IP-bound signature.
    const char* shown = downloadUrl ? downloadUrl : "(local)";
    const char* query = strchr(shown, '?');
    ESP_LOGI(TAG, "Play requested for songId: %s, url: %.*s, startPos: %u ms", songId,
             query ? static_cast<int>(query - shown) : static_cast<int>(strlen(shown)), shown,
             (unsigned int)startPosMs);

    _should_resume_after_session = false;
    _should_play_after_session = false;
    _pendingStartPosMs = 0;

    if (_state != STATE_IDLE) {
        stop();
    }

    strncpy(_activeSongId, songId, sizeof(_activeSongId) - 1);
    _activeSongId[sizeof(_activeSongId) - 1] = '\0';
    _activeDownloadUrl = downloadUrl ? downloadUrl : "";
    _cues.clear();
    _localSource = false;
    _caching = false;

    // Flush buffers before starting new stream
    BufferManager::getInstance().flush(_playbackId);
    BufferManager::getInstance().flush(_storageId);
    BufferManager::getInstance().flush(Buffers::MEDIA_RX_BUF);

    notifyTrackStarted(songId);
    AudioOrchestrator::getInstance().notifyMediaStarted();

    // A full path instead of a URL: a file outside the music cache (a recording).
    const bool byPath = downloadUrl && downloadUrl[0] == '/';
    if (byPath || _storageManager.fileExists(songId)) {
        ESP_LOGI(TAG, "Cache Hit! Playing local file for songId: %s", songId);
        _state = STATE_LOCAL_PLAYBACK;

        if (!(byPath ? _storageManager.openPathForReading(downloadUrl) : _storageManager.openFileForReading(songId))) {
            ESP_LOGE(TAG, "Failed to open local file for reading");
            stopActivePipelines();
            _state = STATE_IDLE;
            EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
                s.media.state = MediaPlaybackState::ERROR_STATE;
                s.media.seekable = false;
                s.media.active_song_id[0] = '\0';
            });
            notifyPlaybackError(songId, -1);
            return;
        }

        _localSource = true;
        size_t headLen = 0;
        uint8_t* head = loadLocalIndex(headLen);
        fillUnknownDuration(songId, _fileDurationMs);
        if (startPosMs > 0) {
            const uint32_t offset = localSeekOffset(startPosMs);
            ESP_LOGI(TAG, "Starting at %u ms: byte %u", (unsigned)startPosMs, (unsigned)offset);
            _storageManager.seekTo(offset);
            BufferManager::getInstance().flush(Buffers::MEDIA_RX_BUF);
            _audioEngine.startAt(startPosMs, false, head, headLen);
        } else {
            _audioEngine.start();
        }
        heap_caps_free(head);
        EmbeddedSysDb::getInstance().mutate([songId, startPosMs](SystemState& s) {
            s.media.state = MediaPlaybackState::PLAYING;
            s.media.seekable = true;
            s.media.output_target = MediaOutputTarget::LOCAL;
            strncpy(s.media.active_song_id, songId, sizeof(s.media.active_song_id) - 1);
            s.media.active_song_id[sizeof(s.media.active_song_id) - 1] = '\0';
            s.media.position_ms = startPosMs;
        });
    } else {
        auto snap = EmbeddedSysDb::getInstance().snapshot();
        // Long tracks (mixes, podcasts) would fill the card; unknown length is treated as long.
        uint32_t durMs = snap.media.duration_ms;
        if (durMs == 0) durMs = static_cast<uint32_t>(StreamManager::urlNumberParam(downloadUrl, "dur") * 1000);
        const bool cacheable = durMs > 0 && durMs <= MAX_CACHE_DURATION_MS;
        bool doCache = snap.media.cache_downloads && (startPosMs == 0) && cacheable;
        if (snap.media.cache_downloads && startPosMs == 0 && !cacheable) {
            ESP_LOGI(TAG, "Not caching %s: duration %u s exceeds the %u s limit or is unknown",
                     songId, (unsigned)(durMs / 1000), (unsigned)(MAX_CACHE_DURATION_MS / 1000));
        }

        _state = STATE_STREAMING_AND_CACHING;
        fillUnknownDuration(songId, durMs);

        if (doCache) {
            ESP_LOGI(TAG, "Cache Miss! Downloading and streaming with caching songId: %s", songId);
            if (!_storageManager.openFileForCaching(songId, static_cast<size_t>(StreamManager::urlNumberParam(downloadUrl, "clen")))) {
                ESP_LOGE(TAG, "Failed to open file for caching");
                stopActivePipelines();
                _state = STATE_IDLE;
                EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
                    s.media.state = MediaPlaybackState::ERROR_STATE;
                    s.media.seekable = false;
                    s.media.active_song_id[0] = '\0';
                });
                notifyPlaybackError(songId, -2);
                return;
            }
            _caching = true;

            _audioEngine.start();
            EmbeddedSysDb::getInstance().mutate([songId](SystemState& s) {
                s.media.state = MediaPlaybackState::PLAYING;
                s.media.seekable = true;
                s.media.output_target = MediaOutputTarget::LOCAL;
                strncpy(s.media.active_song_id, songId, sizeof(s.media.active_song_id) - 1);
                s.media.active_song_id[sizeof(s.media.active_song_id) - 1] = '\0';
                s.media.position_ms = 0;
            });

            if (!_streamManager.beginStreaming(downloadUrl, true, songId)) {
                ESP_LOGE(TAG, "Failed to start streaming");
                stopActivePipelines();
                _state = STATE_IDLE;
                EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
                    s.media.state = MediaPlaybackState::ERROR_STATE;
                    s.media.seekable = false;
                    s.media.active_song_id[0] = '\0';
                });
                notifyPlaybackError(songId, -3);
                return;
            }
        } else {
            ESP_LOGI(TAG, "Streaming songId: %s (startPos: %u ms)", songId, (unsigned int)startPosMs);
            EmbeddedSysDb::getInstance().mutate([songId, startPosMs](SystemState& s) {
                s.media.state = MediaPlaybackState::PLAYING;
                s.media.seekable = true;
                s.media.output_target = MediaOutputTarget::LOCAL;
                strncpy(s.media.active_song_id, songId, sizeof(s.media.active_song_id) - 1);
                s.media.active_song_id[sizeof(s.media.active_song_id) - 1] = '\0';
                s.media.position_ms = startPosMs;
            });

            bool streamOk = false;
            if (startPosMs > 0) {
                // The network task finds the cluster; the decoder skips to the exact time.
                const bool webm = isWebmUrl(downloadUrl);
                _audioEngine.startAt(startPosMs, false, webm ? EBML_MAGIC : nullptr, webm ? sizeof(EBML_MAGIC) : 0);
                streamOk = _streamManager.beginStreamingAt(downloadUrl, startPosMs, songId);
            } else {
                _audioEngine.start();
                streamOk = _streamManager.beginStreaming(downloadUrl, false, songId);
            }

            if (!streamOk) {
                ESP_LOGE(TAG, "Failed to start streaming");
                stopActivePipelines();
                _state = STATE_IDLE;
                EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
                    s.media.state = MediaPlaybackState::ERROR_STATE;
                    s.media.seekable = false;
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
    if (_alarmOwner) return;
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
        s.media.seekable = false;
        s.media.active_song_id[0] = '\0';
    });
}

void NexusPlayer::stopActivePipelines() {
    auto& bm = BufferManager::getInstance();

    // 1. Signal the decoder to stop, then clear the rings it feeds on/into so it
    //    can't be wedged by output backpressure (its throttle) or a full input.
    _audioEngine.stop();
    bm.flush(Buffers::MEDIA_RX_BUF);   // release decoder's PCM-output backpressure
    bm.flush(_playbackId);             // discard pending compressed audio, make room
    bm.flush(_storageId);

    // 2. Wake a decoder that's parked on an empty input ring so it sees the stop
    //    flag and exits (flush alone does not unblock a blocked reader).
    //    The SD writer gets ERROR, not EOF: EOF means "download complete" and would
    //    commit the partial .tmp as the cached track.
    AudioChunkHeader eof_header = {ChunkType::EOF_STREAM, 0};
    AudioChunkHeader abort_header = {ChunkType::ERROR, 0};
    bm.send(_playbackId, &eof_header, sizeof(eof_header));
    bm.send(_storageId, &abort_header, sizeof(abort_header));
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
            if (_alarmOwner) {
                // beginAlarm() took the deferred play/resume into the resume point.
            } else if (_should_play_after_session && (!_pendingDownloadUrl.empty() || _storageManager.fileExists(_pendingSongId.c_str()))) {
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
    std::vector<IPlaybackObserver*> observers;

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
                    // Chosen now: the owner may change before the notification.
                    observers = currentObservers();
                    EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
                        s.media.state = MediaPlaybackState::IDLE;
                        s.media.seekable = false;
                        s.media.active_song_id[0] = '\0';
                    });
                }
            }
        }
    }

    if (trackFinished) {
        // Notify observers (MusicPlaybackService, or the alarm) outside the player lock
        for (auto* obs : observers) {
            if (obs) obs->onTrackFinished(finishedSong);
        }
    }
}

void NexusPlayer::beginAlarm(IPlaybackObserver* alarm) {
    PlayerLock lock(_mutex);
    if (_alarmOwner && _alarmObserver == alarm) return;
    _resume = {};
    const bool playing = _state == STATE_STREAMING_AND_CACHING || _state == STATE_LOCAL_PLAYBACK;
    // Music paused for an assistant session was going to resume when the
    // session ended; the alarm ends the session, so it counts as playing.
    const bool pausedForSession = _state == STATE_PAUSED && _should_resume_after_session;
    if ((playing || pausedForSession) && _activeSongId[0] != '\0') {
        _resume.valid = true;
        _resume.songId = _activeSongId;
        _resume.url = _activeDownloadUrl;
        _resume.positionMs = _audioEngine.getPositionMs();
    } else if (_should_play_after_session && !_pendingSongId.empty()) {
        _resume.valid = true;
        _resume.songId = _pendingSongId;
        _resume.url = _pendingDownloadUrl;
        _resume.positionMs = _pendingStartPosMs;
    }
    if (_resume.valid) {
        ESP_LOGI(TAG, "Alarm takes the player; will resume %s at %u ms", _resume.songId.c_str(),
                 (unsigned)_resume.positionMs);
    }

    stop();   // clears the deferred play/resume flags
    _alarmObserver = alarm;
    _alarmOwner = true;
}

bool NexusPlayer::playAlarm(const char* songId, const char* path, uint32_t startMs) {
    PlayerLock lock(_mutex);
    if (!_alarmOwner || !songId || (!path && !_storageManager.fileExists(songId))) return false;
    play_internal(songId, path, startMs);
    return _state == STATE_LOCAL_PLAYBACK;
}

void NexusPlayer::stopAlarmSong() {
    PlayerLock lock(_mutex);
    if (_alarmOwner) stop();
}

void NexusPlayer::endAlarm(bool restore) {
    PlayerLock lock(_mutex);
    if (!_alarmOwner) return;
    stop();
    _alarmOwner = false;
    _alarmObserver = nullptr;
    ResumePoint r = std::move(_resume);
    _resume = {};
    if (!restore || !r.valid) return;
    const bool local = _storageManager.fileExists(r.songId.c_str());
    if (!local && r.url.empty()) {
        ESP_LOGW(TAG, "Cannot resume %s after the alarm: no file and no stream URL", r.songId.c_str());
        return;
    }
    ESP_LOGI(TAG, "Alarm over; resuming %s at %u ms", r.songId.c_str(), (unsigned)r.positionMs);
    play_internal(r.songId.c_str(), r.url.empty() ? nullptr : r.url.c_str(), r.positionMs);
}

void NexusPlayer::yieldAlarm() {
    if (_alarmOwner && _alarmYield) _alarmYield();
}

uint8_t* NexusPlayer::loadLocalIndex(size_t& headLen) {
    const int64_t t0 = esp_timer_get_time();
    _cues.clear();
    _fileDurationMs = 0;
    headLen = 0;
    size_t want = 4096;
    uint8_t* head = nullptr;
    Media::WebmCues index;
    for (int attempt = 0; attempt < 4; ++attempt) {
        uint8_t* grown = static_cast<uint8_t*>(heap_caps_realloc(head, want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!grown) break;
        head = grown;
        headLen = _storageManager.readHead(head, want);
        index = Media::parseWebmCues(head, headLen);
        if (index.status != Media::CuesStatus::NeedMore || headLen < want) break;
        want = index.need;
    }
    const char* status = index.status == Media::CuesStatus::Found      ? "found"
                         : index.status == Media::CuesStatus::NotFound ? "none"
                         : index.status == Media::CuesStatus::NeedMore ? "cut off"
                                                                       : "not WebM";
    if (index.status == Media::CuesStatus::Found) _cues = std::move(index.cues);
    _fileDurationMs = index.duration_ms;
    ESP_LOGI(TAG, "Seek index: %s, %u cues, %u header bytes, duration %u ms, %lld us", status,
             (unsigned)_cues.size(), (unsigned)headLen, (unsigned)_fileDurationMs,
             (long long)(esp_timer_get_time() - t0));
    if (headLen == 0) {
        heap_caps_free(head);
        return nullptr;
    }
    return head;
}

void NexusPlayer::fillUnknownDuration(const char* songId, uint32_t durationMs) {
    if (durationMs == 0 || EmbeddedSysDb::getInstance().snapshot().media.duration_ms != 0) return;
    EmbeddedSysDb::getInstance().mutate([durationMs](SystemState& s) {
        if (s.media.duration_ms == 0) s.media.duration_ms = durationMs;
    });
    CatalogDB::getInstance().setDurationIfUnknown(songId, durationMs);
    ESP_LOGI(TAG, "Length of %s was unknown: %u ms from the %s", songId, (unsigned)durationMs,
             _localSource ? "file" : "stream URL");
}

uint32_t NexusPlayer::localSeekOffset(uint32_t positionMs) {
    if (!_cues.empty()) return Media::cueAtOrBefore(_cues, positionMs).offset;
    // No index: estimate from the average bitrate, aiming 5 s early so the
    // decoder starts before the target and skips forward to it.
    constexpr uint32_t EARLY_MS = 5000;
    const uint32_t durationMs = EmbeddedSysDb::getInstance().snapshot().media.duration_ms;
    const size_t size = _storageManager.fileSize();
    if (durationMs == 0 || size == 0) return 0;
    const uint32_t aim = positionMs > EARLY_MS ? positionMs - EARLY_MS : 0;
    return static_cast<uint32_t>(uint64_t(aim) * size / durationMs);
}

void NexusPlayer::haltDecoder() {
    auto& bm = BufferManager::getInstance();
    _audioEngine.stop();
    bm.flush(Buffers::MEDIA_RX_BUF);   // release the decoder's output backpressure
    bm.flush(_playbackId);
    // Wake a decoder parked on an empty input ring.
    AudioChunkHeader eof_header = {ChunkType::EOF_STREAM, 0};
    bm.send(_playbackId, &eof_header, sizeof(eof_header));
    if (!_audioEngine.waitUntilStopped()) {
        ESP_LOGW(TAG, "Decode task did not stop for the seek");
    }
    bm.flush(Buffers::MEDIA_RX_BUF);
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

    if (_localSource) {
        // Stop the decoder, move the reader, start again at the new position:
        // nothing read or decoded before the seek reaches the speaker.
        const int64_t t0 = esp_timer_get_time();
        const bool paused = _state == STATE_PAUSED;
        const uint32_t offset = localSeekOffset(positionMs);
        haltDecoder();
        _storageManager.seekTo(offset);
        _audioEngine.startAt(positionMs, paused);
        ESP_LOGI(TAG, "Seek to %u ms: byte %u (%s), set up in %lld us", (unsigned)positionMs, (unsigned)offset,
                 _cues.empty() ? "estimate" : "index", (long long)(esp_timer_get_time() - t0));
        return;
    }

    if (_activeDownloadUrl.empty()) {
        ESP_LOGW(TAG, "Cannot seek: stream URL empty");
        return;
    }
    // Streams: stop the download and the decoder, then download again from
    // the cluster before the target; the decoder skips to the exact time.
    const int64_t t0 = esp_timer_get_time();
    const bool paused = _state == STATE_PAUSED;
    _streamManager.stopStreaming();
    haltDecoder();
    if (_caching) {
        // The decoder reads the partly saved file, which can't seek yet:
        // stop saving and stream directly. The next play from the start saves it.
        ESP_LOGI(TAG, "Seek while saving %s: saving stopped", _activeSongId);
        _storageManager.closeActiveFile();
        _caching = false;
    }
    auto& bm = BufferManager::getInstance();
    bm.flush(_playbackId);
    bm.flush(_storageId);
    _audioEngine.startAt(positionMs, paused);
    _streamManager.beginStreamingAt(_activeDownloadUrl.c_str(), positionMs, _activeSongId);
    ESP_LOGI(TAG, "Seek to %u ms: stream restarted in %lld us", (unsigned)positionMs,
             (long long)(esp_timer_get_time() - t0));
}
