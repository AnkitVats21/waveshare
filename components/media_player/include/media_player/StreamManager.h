#pragma once
#include "HttpClientStream.h"
#include "services/BufferManager.h"
#include "media_player/WebmSeek.h"
#include <functional>
#include <string>
#include <vector>

class StreamManager {
public:
    StreamManager(BufferManager::BufferId playbackId, BufferManager::BufferId storageId);
    ~StreamManager();

    // trackId names the track for a URL renewal (see setUrlRenewer).
    bool beginStreaming(const char* url, bool cacheMode = false, const char* trackId = nullptr);
    bool beginStreamingFrom(const char* url, uint32_t byteOffset, bool cacheMode = false,
                            const char* trackId = nullptr);
    // Streams from the cluster at or before targetMs. The network task finds
    // it in the stream's index, fetching the index first if it is not known
    // yet, or estimates it from the URL's clen/dur, aiming 5 s early.
    bool beginStreamingAt(const char* url, uint32_t targetMs, const char* trackId = nullptr);
    void stopStreaming();
    bool isStreaming() const { return _isStreaming; }

    // While held, the network task stops reading: TCP stops the server, so
    // the download uses no bandwidth or decryption (an assistant session
    // gets the link to itself). Released, it reads on from where it was; if
    // the server dropped the idle connection meanwhile, the reconnect path
    // resumes at the same byte. Stays set across tracks until released.
    void hold(bool on) { _held = on; }

    // Numeric query parameter of a stream URL (googlevideo carries
    // dur=<seconds> and clen=<bytes>); 0 when absent.
    static double urlNumberParam(const char* url, const char* key);

    // Resolves a track's stream URL again when the server rejects the one in
    // use (expired). Runs on the network task and may take seconds; returns
    // false on failure or once cancelled() is true. Set once at startup.
    using UrlRenewer = std::function<bool(const std::string& trackId, std::string& outUrl,
                                          const std::function<bool()>& cancelled)>;
    void setUrlRenewer(UrlRenewer renewer) { _urlRenewer = std::move(renewer); }

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
    volatile bool _held = false;
    bool _taskCreatedWithCaps = false;
    
    bool _startAtTime = false;   // beginStreamingAt: resolve _targetMs to an offset
    uint32_t _targetMs = 0;
    bool start(const char* url, bool cacheMode, uint32_t byteOffset, bool atTime, uint32_t targetMs,
               const char* trackId);

    // The URL the player passed is the key for the current track; the two
    // below stand in for it while that track plays (seeks and reconnects
    // pass the same key), in RAM only, cleared when the track changes.
    std::string _trackId;
    std::string _renewedUrl;     // a fresh URL for the key, after it expired
    std::string _redirectedUrl;  // where the last redirect led (skips the 302)
    UrlRenewer _urlRenewer;
    // Opens the stream at offset: the remembered redirect first, then the
    // current URL, renewing it once if the server rejects it as expired.
    bool openStream(uint32_t offset);
    bool openAndRemember(const std::string& url, uint32_t offset);
    bool renewUrl();
    const std::string& currentUrl() const { return _renewedUrl.empty() ? _url : _renewedUrl; }
    // Waits ms while streaming continues; false if stopped meanwhile.
    bool waitWhileStreaming(uint32_t ms);

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
