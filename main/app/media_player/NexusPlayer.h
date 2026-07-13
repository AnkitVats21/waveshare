#pragma once
#include "services/BufferManager.h"
#include "StorageManager.h"
#include "StreamManager.h"
#include "AudioEngine.h"

// Declare the playback and storage buffers for NexusPlayer
DECLARE_BUFFER(PLAYER_BUF, "player_buf", 512 * 1024)
DECLARE_BUFFER(STREAM_BUF, "stream_buf", 256 * 1024)

enum PlayerState { 
    STATE_IDLE, 
    STATE_STREAMING_AND_CACHING, 
    STATE_LOCAL_PLAYBACK, 
    STATE_PAUSED 
};

class NexusPlayer {
public:
    static NexusPlayer& getInstance();
    NexusPlayer(BufferManager::BufferId playbackId, BufferManager::BufferId storageId);
    ~NexusPlayer();

    bool begin();
    
    void play(const char* songId, const char* downloadUrl);
    void pause();
    void resume();
    void stop();
    
    PlayerState getState() { return _state; }

private:
    PlayerState _state = STATE_IDLE;
    char _activeSongId[64] = {0};
    
    BufferManager::BufferId _playbackId;
    BufferManager::BufferId _storageId;
    
    StorageManager   _storageManager;
    StreamManager    _streamManager;
    AudioEngine      _audioEngine;
    
    void stopActivePipelines();
};
