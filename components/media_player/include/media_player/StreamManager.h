#pragma once
#include "HttpClientStream.h"
#include "services/BufferManager.h"
#include "media_player/WebmSeek.h"
#include <string>
#include <vector>

class StreamManager {
public:
    StreamManager(BufferManager::BufferId playbackId, BufferManager::BufferId storageId);
    ~StreamManager();

    bool beginStreaming(const char* url, bool cacheMode = false);
    bool beginStreamingFrom(const char* url, uint32_t byteOffset, bool cacheMode = false);
    // Streams from the cluster at or before targetMs. The network task finds
    // it in the stream's index, fetching the index first if it is not known
    // yet, or estimates it from the URL's clen/dur, aiming 5 s early.
    bool beginStreamingAt(const char* url, uint32_t targetMs);
    void stopStreaming();
    bool isStreaming() const { return _isStreaming; }

    // Numeric query parameter of a stream URL (googlevideo carries
    // dur=<seconds> and clen=<bytes>); 0 when absent.
    static double urlNumberParam(const char* url, const char* key);

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
    
    bool _startAtTime = false;   // beginStreamingAt: resolve _targetMs to an offset
    uint32_t _targetMs = 0;
    bool start(const char* url, bool cacheMode, uint32_t byteOffset, bool atTime, uint32_t targetMs);

    // The stream's seek index (WebM Cues), read from its first bytes while it
    // streams from the start, or by a separate request. Touched only by the
    // network task, or while none runs; kept while the same URL restarts (seeks).
    std::vector<Media::CuePoint> _cues;
    std::string _cuesUrl;        // the URL _cues belong to
    bool _indexKnown = false;    // found, or known to be missing
    void fetchIndex();
    uint32_t resolveOffset(uint32_t targetMs, const char*& how);
    // The first bytes of a stream from the start, kept until the index is read.
    uint8_t* _head = nullptr;
    size_t _headLen = 0;
    size_t _headWant = 0;
    void captureHead(const uint8_t* data, size_t len);
    void endCapture();
    // Parses a head and stores the index. Returns false while more bytes
    // would get further, with `want` set to how many.
    bool takeIndex(const uint8_t* head, size_t len, size_t& want, const char* how);

    static void networkTaskThunk(void* pvParameters);
    void runStreamLoop();
};
