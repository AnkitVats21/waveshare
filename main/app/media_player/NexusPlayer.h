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
    
    void play(const char* songId, const char* downloadUrl);
    void pause();
    void resume();
    void stop();
    
    void playAlert(AlertType type);
    
    PlayerState getState() { return _state; }

    // ReactorTask interface
    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

private:
    PlayerState _state = STATE_IDLE;
    char _activeSongId[64] = {0};
    uint8_t* _savedPcmBuffer = nullptr;
    size_t _savedPcmLen = 0;
    
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

    // Deferred playback state cache
    bool _should_play_after_session = false;
    char _pendingSongId[64] = {0};
    char _pendingDownloadUrl[256] = {0};

    void pause_internal();
    void resume_internal();
    void play_internal(const char* songId, const char* downloadUrl);
};
