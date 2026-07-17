#pragma once
#include "services/BufferManager.h"
#include "StorageManager.h"
#include "StreamManager.h"
#include "AudioEngine.h"
#include "common/ReactorTask.h"
#include "freertos/semphr.h"

// Declare the playback and storage buffers for NexusPlayer
DECLARE_BUFFER(PLAYER_BUF, "player_buf", 512 * 1024)
DECLARE_BUFFER(STREAM_BUF, "stream_buf", 256 * 1024)

enum PlayerState { 
    STATE_IDLE, 
    STATE_STREAMING_AND_CACHING, 
    STATE_LOCAL_PLAYBACK, 
    STATE_PAUSED 
};

class NexusPlayer : public ReactorTask {
public:
    static NexusPlayer& getInstance();
    NexusPlayer(BufferManager::BufferId playbackId, BufferManager::BufferId storageId);
    virtual ~NexusPlayer() override;

    bool begin();
    
    // Primary playback controls (called by MQTT "play" response or Gemini PLAY skill)
    void play(const char* songId, const char* downloadUrl);
    void pause();
    void resume();
    void stop();

    // Device-level skip — called by Gemini NEXT skill, hardware button, or external cmd:next.
    // The player manages the queue and decides when to request the next song from the server.
    void playNext();

    // Called by MqttService when a "play_next" server response arrives
    void setNextSong(const char* songId, const char* url);
    
    void playAlert(AlertType type);
    
    PlayerState getState() { return _state; }

    // ReactorTask interface
    void onStateChanged(ComponentMask changed, const SystemState& snap) override;
    void run() override;

private:
    // ── Current playback slot ──────────────────────────────────────────────
    PlayerState _state = STATE_IDLE;
    char _activeSongId[64] = {0};
    uint8_t* _savedPcmBuffer = nullptr;
    size_t _savedPcmLen = 0;
    
    BufferManager::BufferId _playbackId;
    BufferManager::BufferId _storageId;
    
    StorageManager   _storageManager;
    StreamManager    _streamManager;
    AudioEngine      _audioEngine;

    // ── Next-song slot (prefetch) ──────────────────────────────────────────
    enum class PrefetchState { NONE, WAITING, DOWNLOADING, CACHED };

    char          _nextSongId[64]   = {0};
    char          _nextSongUrl[256] = {0};
    PrefetchState _prefetchState    = PrefetchState::NONE;

    // ── Guard flags ────────────────────────────────────────────────────────
    // Prevents sending duplicate "next" requests to the server
    bool _nextRequestSent          = false;
    // Set when skip is pressed but next slot is empty — routes next play_next as current song
    bool _waitingForPlayNextAsPlay = false;
    // Tracks whether onDownloadComplete has already been handled for this song
    bool _downloadWasComplete      = false;

    // ── Reactor task state & thread safety ────────────────────────────────
    SemaphoreHandle_t _mutex = nullptr;
    bool _session_active = false;
    bool _should_resume_after_session = false;

    // Deferred playback state cache (used when assistant session is active)
    bool _should_play_after_session = false;
    char _pendingSongId[64] = {0};
    char _pendingDownloadUrl[256] = {0};

    // ── Internal methods ───────────────────────────────────────────────────
    void stopActivePipelines();
    void pause_internal();
    void resume_internal();
    void play_internal(const char* songId, const char* downloadUrl);
    void checkPlaybackFinished();

    // Called when the current song's HTTP download finishes — triggers next-song request
    void onDownloadComplete();
    // Swap next slot into current slot and begin playback immediately
    void promoteNextToCurrent();
    // Clear all next-slot state (including stopping any in-progress prefetch)
    void clearNextSlot();
    // Publish {"cmd":"next"} with guard to avoid duplicates
    void requestNextSong();
};
