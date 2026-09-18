#pragma once
#include "HttpClientStream.h"
#include "services/BufferManager.h"
#include <string>

class StreamManager {
public:
    StreamManager(BufferManager::BufferId playbackId, BufferManager::BufferId storageId);
    ~StreamManager();

    bool beginStreaming(const char* url, bool cacheMode = false);
    bool beginStreamingFrom(const char* url, uint32_t byteOffset, bool cacheMode = false);
    void stopStreaming();
    bool isStreaming() const { return _isStreaming; }

private:
    BufferManager& _bm;
    BufferManager::BufferId _playbackId;
    BufferManager::BufferId _storageId;
    
    HttpClientStream _http;
    std::string _url;
    bool _cacheMode = false;
    uint32_t _startByteOffset = 0;
    TaskHandle_t _networkTaskHandle = nullptr;
    volatile bool _isStreaming = false;
    bool _taskCreatedWithCaps = false;
    
    static void networkTaskThunk(void* pvParameters);
    void runStreamLoop();
};
