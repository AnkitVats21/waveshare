#pragma once
#include <cstdio>
#include "services/BufferManager.h"
#include "services/storage/StorageService.h"

class StorageManager {
public:
    StorageManager(BufferManager::BufferId playbackId, BufferManager::BufferId storageId);
    ~StorageManager();

    bool fileExists(const char* songId);
    
    // Cache Miss Path (concurrent download, write and progressive read)
    bool openFileForCaching(const char* songId);
    
    // Cache Hit Path (local file read and playback)
    bool openFileForReading(const char* songId);
    
    void closeActiveFile();
    void setDownloadCompleteSignal(bool complete) { _downloadComplete = complete; }

    // Prefetch Path (writer-only — downloads next song silently to SD)
    // Must only be called after the main writer task has exited (after onDownloadComplete)
    bool beginPrefetch(const char* songId);
    void stopPrefetch();
    bool isPrefetchComplete() const { return _prefetchComplete; }

private:
    BufferManager& _bm;
    Services::StorageService& _storageService;
    
    BufferManager::BufferId _playbackId;
    BufferManager::BufferId _storageId;
    
    FILE* _writeStream = nullptr;
    FILE* _readStream = nullptr;
    
    char _currentSongId[64] = {0};
    
    volatile bool _downloadComplete = false;
    volatile size_t _bytesWritten = 0;
    
    volatile bool _writerTaskRunning = false;
    volatile bool _readerTaskRunning = false;
    
    TaskHandle_t _writerTaskHandle = nullptr;
    TaskHandle_t _readerTaskHandle = nullptr;
    
    bool _isWritingMode = false;

    // Prefetch state (writer-only, no reader, no AudioEngine)
    volatile bool _prefetchWriterRunning = false;
    volatile bool _prefetchComplete = false;
    char _prefetchSongId[64] = {0};
    TaskHandle_t _prefetchWriterHandle = nullptr;
    
    static void sdWriterTaskThunk(void* pvParameters);
    static void sdReaderTaskThunk(void* pvParameters);
    static void sdPrefetchWriterTaskThunk(void* pvParameters);
    
    void runWriterTaskLoop();
    void runReaderTaskLoop();
    void runPrefetchWriterLoop();
};