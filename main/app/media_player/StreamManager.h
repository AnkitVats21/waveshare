#pragma once
#include "HttpClientStream.h"
#include "services/BufferManager.h"
#include <string>

class StorageManager; // Forward declaration

class StreamManager {
public:
    StreamManager(BufferManager::BufferId playbackId, BufferManager::BufferId storageId, StorageManager& storageMngr);
    ~StreamManager();

    bool beginStreaming(const char* url);
    void stopStreaming();
    bool isStreaming() const { return _isStreaming; }
    bool isDownloadComplete() const { return _downloadComplete; }

private:
    BufferManager& _bm;
    BufferManager::BufferId _playbackId;
    BufferManager::BufferId _storageId;
    StorageManager& _storage;
    
    HttpClientStream _http;
    std::string _url;
    TaskHandle_t _networkTaskHandle = nullptr;
    volatile bool _isStreaming = false;
    volatile bool _downloadComplete = false;
    
    static void networkTaskThunk(void* pvParameters);
    void runStreamLoop();
};
