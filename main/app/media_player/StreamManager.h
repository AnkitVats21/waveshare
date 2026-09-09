#pragma once
#include "HttpClientStream.h"
#include "services/BufferManager.h"
#include <string>

class StreamManager {
public:
    StreamManager(BufferManager::BufferId playbackId, BufferManager::BufferId storageId);
    ~StreamManager();

    bool beginStreaming(const char* url, bool cacheMode = false);
    void stopStreaming();
    bool isStreaming() const { return _isStreaming; }

private:
    BufferManager& _bm;
    BufferManager::BufferId _playbackId;
    BufferManager::BufferId _storageId;
    
    HttpClientStream _http;
    std::string _url;
    bool _cacheMode = false;
    TaskHandle_t _networkTaskHandle = nullptr;
    volatile bool _isStreaming = false;
    
    static void networkTaskThunk(void* pvParameters);
    void runStreamLoop();
};
