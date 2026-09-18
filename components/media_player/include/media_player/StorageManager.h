#pragma once
#include <cstdio>
#include "core_sysdb/BufferManager.h"
#include "media_player/IStorageService.h"

class StorageManager {
public:
    StorageManager(BufferManager::BufferId playbackId, BufferManager::BufferId storageId);
    ~StorageManager();

    void setStorageService(IStorageService* storageService) { _storageService = storageService; }

    bool fileExists(const char* songId);
    bool deleteFile(const char* songId);
    
    // Cache Miss Path (concurrent download, write and progressive read)
    bool openFileForCaching(const char* songId);
    
    // Cache Hit Path (local file read and playback)
    bool openFileForReading(const char* songId);
    
    // Seek active playback stream
    bool seekTo(uint32_t byteOffset);
    
    void closeActiveFile();
    void setDownloadCompleteSignal(bool complete) { _downloadComplete = complete; }

private:
    SemaphoreHandle_t _streamMutex = nullptr;
    bool getValidCachedPath(const char* songId, char* outPath, size_t maxLen);

    BufferManager& _bm;
    IStorageService* _storageService = nullptr;
    
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
    
    static void sdWriterTaskThunk(void* pvParameters);
    static void sdReaderTaskThunk(void* pvParameters);
    
    void runWriterTaskLoop();
    void runReaderTaskLoop();
};