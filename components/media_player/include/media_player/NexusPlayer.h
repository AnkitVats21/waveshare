#pragma once
#include "services/BufferManager.h"
#include "StorageManager.h"
#include "StreamManager.h"
#include "AudioEngine.h"
#include "CatalogDB.h"
#include "WebmSeek.h"
#include "IPlaybackObserver.h"
#include "common/ReactorTask.h"
#include "freertos/semphr.h"
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "app/audio/AudioOrchestrator.h"

// Declare the playback and storage buffers for NexusPlayer
DECLARE_BUFFER(PLAYER_BUF, "player_buf", 512 * 1024)
DECLARE_BUFFER(STREAM_BUF, "stream_buf", 256 * 1024)

enum PlayerState { 
    STATE_IDLE, 
    STATE_STREAMING_AND_CACHING, 
    STATE_LOCAL_PLAYBACK, 
    STATE_PAUSED 
};

class NexusPlayer : public ReactorTask, public IAudioFocusObserver {
public:
    // Streams longer than this are played but never written to the SD cache.
    static constexpr uint32_t MAX_CACHE_DURATION_MS = 10 * 60 * 1000;

    static NexusPlayer& getInstance();
    NexusPlayer(BufferManager::BufferId playbackId, BufferManager::BufferId storageId);
    virtual ~NexusPlayer() override;

    bool begin();
    
    void play(const char* songId, const char* downloadUrl);
    void playAt(const char* songId, const char* downloadUrl, uint32_t startPosMs);
    void pause();
    void resume();
    void stop();
    void seekTo(uint32_t positionMs);
    uint32_t getPositionMs() const;
    
    PlayerState getState() { return _state; }

    // ── Alarm ownership (docs/alarm-design.md) ─────────────────────────────
    // While an alarm owns the player, playback events go only to `alarm`,
    // focus changes and music play requests are ignored, and the music that
    // was playing is remembered for endAlarm(true). Nothing happens if
    // `alarm` owns it already (what plays keeps playing).
    void beginAlarm(IPlaybackObserver* alarm);
    // Plays a local (cached) track for the alarm; false if it can't start.
    // path: play that file (an uploaded alarm tone) instead of the cached song.
    bool playAlarm(const char* songId, const char* path = nullptr, uint32_t startMs = 0);
    // Stops the alarm song, keeping ownership (snooze, built-in fallback).
    void stopAlarmSong();
    // Gives the player back; with restore, the remembered music resumes.
    void endAlarm(bool restore);
    bool alarmOwned() const { return _alarmOwner; }
    // MusicPlaybackService calls yieldAlarm() before a user music command; the
    // handler (AlarmService) stops the alarm without restoring the old music.
    void setAlarmYieldHandler(std::function<void()> handler) { _alarmYield = std::move(handler); }
    // Resolves a track's stream URL again when the one playing has expired
    // (a long pause); called on the network task. Set once at startup.
    void setUrlRenewer(StreamManager::UrlRenewer renewer) { _streamManager.setUrlRenewer(std::move(renewer)); }
    void yieldAlarm();
    StorageManager& getStorageManager() { return _storageManager; }

    // Observer Pattern registration
    void addObserver(IPlaybackObserver* observer);
    void removeObserver(IPlaybackObserver* observer);

    // IAudioFocusObserver interface
    void onAudioFocusChange(AudioTrack track, FocusEvent event) override;

    // ReactorTask interface
    void onStateChanged(ComponentMask changed, const SystemState& snap) override;
    void run() override;

private:
    PlayerState _state = STATE_IDLE;
    char _activeSongId[64] = {0};
    
    BufferManager::BufferId _playbackId;
    BufferManager::BufferId _storageId;
    
    StorageManager   _storageManager;
    StreamManager    _streamManager;
    AudioEngine      _audioEngine;
    
    void stopActivePipelines();

    // Reactor task state & thread safety
    SemaphoreHandle_t _mutex = nullptr;
    bool _session_active = false;
    bool _should_resume_after_session = false;

    // Deferred playback state cache (dynamic string to avoid URL truncation)
    bool _should_play_after_session = false;
    std::string _pendingSongId;
    std::string _pendingDownloadUrl;
    uint32_t _pendingStartPosMs = 0;

    // Registered playback lifecycle observers
    std::vector<IPlaybackObserver*> _observers;

    std::string _activeDownloadUrl;

    // Local playback: the file's seek index (WebM Cues), empty if it has none.
    bool _localSource = false;
    // Streaming while saving to the card (the decoder reads the partial file).
    bool _caching = false;
    std::vector<Media::CuePoint> _cues;
    // The local file's length from its header, 0 if it has none.
    uint32_t _fileDurationMs = 0;
    // Publishes a length found by the player when the track's is unknown,
    // and saves it with the library entry.
    void fillUnknownDuration(const char* songId, uint32_t durationMs);
    // Reads the index; returns the head bytes (PSRAM, caller frees) for the
    // decoder to identify the format, or nullptr.
    uint8_t* loadLocalIndex(size_t& headLen);
    // Where to start reading for a seek to positionMs in the local file.
    uint32_t localSeekOffset(uint32_t positionMs);
    // Stops the decode task and empties the rings around it, keeping the
    // decoder for startAt.
    void haltDecoder();

    // Alarm ownership
    volatile bool _alarmOwner = false;
    IPlaybackObserver* _alarmObserver = nullptr;
    std::function<void()> _alarmYield;
    struct ResumePoint {
        bool valid = false;
        std::string songId;
        std::string url;
        uint32_t positionMs = 0;
    } _resume;
    // Observers for the current owner, copied under the lock.
    std::vector<IPlaybackObserver*> currentObservers();

    void pause_internal();
    void resume_internal();
    void play_internal(const char* songId, const char* downloadUrl, uint32_t startPosMs = 0);
    void checkPlaybackFinished();
    void notifyTrackStarted(const char* songId);
    void notifyTrackFinished(const char* songId);
    void notifyPlaybackError(const char* songId, int err);
};
