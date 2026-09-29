#pragma once
#include "core_sysdb/BufferManager.h"
#include "sd_storage/File.h"
#include <cstring>

class StorageManager {
public:
    StorageManager(BufferManager::BufferId playbackId, BufferManager::BufferId storageId);
    ~StorageManager();

    bool fileExists(const char* songId);
    bool deleteFile(const char* songId);
    // True while songId is being downloaded (its .tmp is in use), until the
    // file is committed. The commit happens as soon as the download ends,
    // while the track may still be playing from the file.
    bool isCaching(const char* songId) const {
        return _isWritingMode && !_committed && songId && strcmp(_currentSongId, songId) == 0;
    }
    
    // Cache Miss Path (concurrent download, write and progressive read).
    // expectedBytes (0 = unknown) guards the commit against truncated downloads.
    bool openFileForCaching(const char* songId, size_t expectedBytes = 0);
    
    // Cache Hit Path (local file read and playback)
    bool openFileForReading(const char* songId);
    // A file outside the music cache (a recording), by its full path: no
    // size check, never deleted.
    bool openPathForReading(const char* path);
    
    // Moves local playback to byteOffset. The playback ring is flushed and a
    // chunk read before the move is dropped, so everything after the call
    // comes from the new position; a reader that already hit EOF restarts.
    bool seekTo(uint32_t byteOffset);

    // Reads up to `len` bytes from the start of the local file (its seek
    // index) without moving playback. Returns the bytes read.
    size_t readHead(uint8_t* dst, size_t len);
    size_t fileSize();
    
    void closeActiveFile();
    void setDownloadCompleteSignal(bool complete) { _downloadComplete = complete; }

private:
    SemaphoreHandle_t _streamMutex = nullptr;
    bool getValidCachedPath(const char* songId, char* outPath, size_t maxLen);
    bool spawnReader();
    // Renames the finished .tmp to its final name and records it in the
    // library. Needs both the writer's and the reader's handles closed.
    bool commitCache();
    // Local reader: sends a chunk unless a seek moved the file since it was read.
    enum class Send { Sent, Moved, Stopped };
    Send sendUnlessMoved(const uint8_t* buf, size_t len, uint32_t gen);

    BufferManager& _bm;
    BufferManager::BufferId _playbackId;
    BufferManager::BufferId _storageId;
    
    sd_storage::File _writeFile;
    sd_storage::File _readFile;
    
    char _currentSongId[64] = {0};
    
    volatile bool _downloadComplete = false;
    volatile size_t _bytesWritten = 0;
    size_t _expectedBytes = 0;
    
    volatile bool _writerTaskRunning = false;
    volatile bool _readerTaskRunning = false;
    
    TaskHandle_t _writerTaskHandle = nullptr;
    TaskHandle_t _readerTaskHandle = nullptr;
    
    bool _isWritingMode = false;
    // The .tmp was renamed to its final name (the reader then follows the final file).
    volatile bool _committed = false;

    // Bumped by seekTo (under _streamMutex); the local reader drops chunks
    // read under an older value.
    uint32_t _readGen = 0;
    // The local reader sent EOF and is exiting; seekTo restarts it.
    bool _readerAtEof = false;
    
    static void sdWriterTaskThunk(void* pvParameters);
    static void sdReaderTaskThunk(void* pvParameters);
    
    void runWriterTaskLoop();
    void runReaderTaskLoop();
};