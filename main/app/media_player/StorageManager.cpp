#include "StorageManager.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <sys/stat.h>
#include <sys/unistd.h>
#include <cstring>
#include <algorithm>
#include <cerrno>
#include "PlayerTypes.h"

static const char* TAG = "StorageManager";

StorageManager::StorageManager(BufferManager::BufferId playbackId, BufferManager::BufferId storageId)
    : _bm(BufferManager::getInstance()),
      _storageService(Services::StorageService::getInstance()),
      _playbackId(playbackId),
      _storageId(storageId) {}

StorageManager::~StorageManager() {
    closeActiveFile();
}

bool StorageManager::fileExists(const char* songId) {
    if (!songId) return false;
    char path[128];
    snprintf(path, sizeof(path), "/sdcard/music/%s.ogg", songId);
    return _storageService.fileExists(path);
}

bool StorageManager::openFileForCaching(const char* songId) {
    if (!songId) return false;
    closeActiveFile();

    // Create the /sdcard/music directory if it doesn't exist
    struct stat st;
    if (stat("/sdcard/music", &st) != 0) {
        if (mkdir("/sdcard/music", 0755) != 0) {
            ESP_LOGW(TAG, "Failed to create music directory (might already exist): %d", errno);
        }
    }

    char tempPath[128];
    snprintf(tempPath, sizeof(tempPath), "/sdcard/music/%s.ogg.tmp", songId);

    ESP_LOGI(TAG, "Opening cache stream at: %s", tempPath);
    _writeStream = _storageService.openStream(tempPath, "wb");
    if (!_writeStream) {
        ESP_LOGE(TAG, "Failed to open cache stream for writing: %s", tempPath);
        return false;
    }

    strncpy(_currentSongId, songId, sizeof(_currentSongId) - 1);
    _currentSongId[sizeof(_currentSongId) - 1] = '\0';
    _downloadComplete = false;
    _bytesWritten = 0;
    _isWritingMode = true;

    _writerTaskRunning = true;
    _readerTaskRunning = true;

    // Spawn concurrent SD Writer task
    BaseType_t ret = xTaskCreatePinnedToCore(
        sdWriterTaskThunk, "sd_writer_task", 4096, this,
        4, &_writerTaskHandle, 1
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn sd_writer_task");
        closeActiveFile();
        return false;
    }

    // Spawn concurrent SD Reader task
    ret = xTaskCreatePinnedToCore(
        sdReaderTaskThunk, "sd_reader_task", 4096, this,
        4, &_readerTaskHandle, 1
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn sd_reader_task");
        closeActiveFile();
        return false;
    }

    return true;
}

bool StorageManager::openFileForReading(const char* songId) {
    if (!songId) return false;
    closeActiveFile();

    char path[128];
    snprintf(path, sizeof(path), "/sdcard/music/%s.ogg", songId);

    ESP_LOGI(TAG, "Opening local playback stream at: %s", path);
    _readStream = _storageService.openStream(path, "rb");
    if (!_readStream) {
        ESP_LOGE(TAG, "Failed to open playback stream: %s", path);
        return false;
    }

    strncpy(_currentSongId, songId, sizeof(_currentSongId) - 1);
    _currentSongId[sizeof(_currentSongId) - 1] = '\0';
    _downloadComplete = true; // Local playback is already complete
    _isWritingMode = false;
    _readerTaskRunning = true;

    // Spawn concurrent SD Reader task
    BaseType_t ret = xTaskCreatePinnedToCore(
        sdReaderTaskThunk, "sd_reader_task", 4096, this,
        4, &_readerTaskHandle, 1
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn sd_reader_task");
        closeActiveFile();
        return false;
    }

    return true;
}

void StorageManager::closeActiveFile() {
    ESP_LOGI(TAG, "Cleaning up active file and task handles");

    // Signal tasks to stop
    _writerTaskRunning = false;
    _readerTaskRunning = false;

    // Wait for writer task to exit
    while (_writerTaskHandle != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Wait for reader task to exit
    while (_readerTaskHandle != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Safely close handles
    if (_writeStream) {
        _storageService.closeStream(_writeStream);
        _writeStream = nullptr;
    }
    if (_readStream) {
        _storageService.closeStream(_readStream);
        _readStream = nullptr;
    }

    // If caching, finalize or clean up
    if (_isWritingMode && _currentSongId[0] != '\0') {
        char tempPath[128];
        char targetPath[128];
        snprintf(tempPath, sizeof(tempPath), "/sdcard/music/%s.ogg.tmp", _currentSongId);
        snprintf(targetPath, sizeof(targetPath), "/sdcard/music/%s.ogg", _currentSongId);

        if (_downloadComplete) {
            ESP_LOGI(TAG, "Download complete. Committing cache to target: %s", targetPath);
            if (_storageService.fileExists(targetPath)) {
                _storageService.deleteFile(targetPath);
            }
            int ret = rename(tempPath, targetPath);
            if (ret != 0) {
                ESP_LOGE(TAG, "Failed to commit cached file (rename error %d)", errno);
            } else {
                ESP_LOGI(TAG, "Successfully committed cache file: %s", targetPath);
            }
        } else {
            ESP_LOGI(TAG, "Download incomplete or aborted. Cleaning up temp cache: %s", tempPath);
            if (_storageService.fileExists(tempPath)) {
                _storageService.deleteFile(tempPath);
            }
        }
    }

    _isWritingMode = false;
    _downloadComplete = false;
    _bytesWritten = 0;
    _currentSongId[0] = '\0';
}

void StorageManager::sdWriterTaskThunk(void* pvParameters) {
    static_cast<StorageManager*>(pvParameters)->runWriterTaskLoop();
}

void StorageManager::sdReaderTaskThunk(void* pvParameters) {
    static_cast<StorageManager*>(pvParameters)->runReaderTaskLoop();
}

void StorageManager::runWriterTaskLoop() {
    ESP_LOGI(TAG, "Writer Task running on Core 1");

    while (_writerTaskRunning) {
        size_t rx_bytes = 0;
        void* rx_ptr = _bm.receive(_storageId, &rx_bytes, pdMS_TO_TICKS(100));
        if (rx_ptr == nullptr) {
            continue;
        }

        AudioChunkHeader* chunk = reinterpret_cast<AudioChunkHeader*>(rx_ptr);
        if (chunk->type == ChunkType::EOF_STREAM) {
            ESP_LOGI(TAG, "Writer Task: Received EOF signal");
            _downloadComplete = true;
            _bm.returnItem(_storageId, rx_ptr);
            break;
        } else if (chunk->type == ChunkType::ERROR) {
            ESP_LOGE(TAG, "Writer Task: Received ERROR signal");
            _bm.returnItem(_storageId, rx_ptr);
            break;
        }

        if (chunk->type == ChunkType::DATA && chunk->size > 0) {
            uint8_t* payload = reinterpret_cast<uint8_t*>(chunk) + sizeof(AudioChunkHeader);
            size_t written = _storageService.writeStream(_writeStream, payload, chunk->size);
            if (written != chunk->size) {
                ESP_LOGE(TAG, "Writer Task: Disk write error! Expected %u, wrote %u", (unsigned)chunk->size, (unsigned)written);
            } else {
                fflush(_writeStream);
                fsync(fileno(_writeStream));
                _bytesWritten += written;
            }
        }

        _bm.returnItem(_storageId, rx_ptr);
    }

    ESP_LOGI(TAG, "Writer Task exiting");
    _writerTaskRunning = false;
    _writerTaskHandle = nullptr;
    vTaskDelete(NULL);
}

void StorageManager::runReaderTaskLoop() {
    ESP_LOGI(TAG, "Reader Task running on Core 1");

    // Pre-allocate read buffer in PSRAM to prevent stack overflows (32KB payload)
    size_t allocSize = sizeof(AudioChunkHeader) + AUDIO_CHUNK_SIZE;
    uint8_t* read_buf = static_cast<uint8_t*>(heap_caps_malloc(allocSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!read_buf) {
        ESP_LOGE(TAG, "Reader Task: Failed to allocate read buffer in PSRAM!");
        _readerTaskRunning = false;
        _readerTaskHandle = nullptr;
        vTaskDelete(NULL);
        return;
    }

    AudioChunkHeader* header = reinterpret_cast<AudioChunkHeader*>(read_buf);
    uint8_t* payload = read_buf + sizeof(AudioChunkHeader);

    size_t read_pos = 0;
    FILE* tempFile = nullptr;

    char tempPath[128];
    if (_isWritingMode) {
        snprintf(tempPath, sizeof(tempPath), "/sdcard/music/%s.ogg.tmp", _currentSongId);
    }

    while (_readerTaskRunning) {
        if (_isWritingMode) {
            // Progressive Cache Reading
            if (read_pos >= _bytesWritten) {
                if (_downloadComplete) {
                    // Download is done, and we have read everything
                    ESP_LOGI(TAG, "Reader Task: Cache Hit EOF reached dynamically");
                    header->type = ChunkType::EOF_STREAM;
                    header->size = 0;
                    _bm.send(_playbackId, read_buf, sizeof(AudioChunkHeader), portMAX_DELAY);
                    break;
                } else {
                    // Caching is active but reader caught up. Close temp stream to allow flush/sync commits,
                    // delay, and reopen to pick up new writes.
                    if (tempFile) {
                        fclose(tempFile);
                        tempFile = nullptr;
                    }
                    vTaskDelay(pdMS_TO_TICKS(100));
                    continue;
                }
            }

            // Open/reopen the temp file if not currently open
            if (tempFile == nullptr) {
                tempFile = fopen(tempPath, "rb");
                if (tempFile == nullptr) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                    continue;
                }
                fseek(tempFile, read_pos, SEEK_SET);
            }

            // Read the next chunk up to the current write threshold
            size_t bytes_to_read = std::min(AUDIO_CHUNK_SIZE, _bytesWritten - read_pos);
            if (bytes_to_read > 0) {
                size_t read_bytes = fread(payload, 1, bytes_to_read, tempFile);
                if (read_bytes > 0) {
                    header->type = ChunkType::DATA;
                    header->size = read_bytes;
                    // Blocks if PLAYER_BUF is full, regulating progressive reading
                    if (_bm.send(_playbackId, read_buf, sizeof(AudioChunkHeader) + read_bytes, pdMS_TO_TICKS(100))) {
                        read_pos += read_bytes;
                    } else {
                        // Reseek to retry sending
                        fseek(tempFile, read_pos, SEEK_SET);
                    }
                } else {
                    // Stale file size read? Wait/reopen
                    fclose(tempFile);
                    tempFile = nullptr;
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        } else {
            // Standard Local Cache Hit Playback
            size_t read_bytes = _storageService.readStream(_readStream, payload, AUDIO_CHUNK_SIZE);
            if (read_bytes > 0) {
                header->type = ChunkType::DATA;
                header->size = read_bytes;
                // Wait indefinitely if player buffer is full to enforce backpressure
                bool sent = false;
                while (_readerTaskRunning && !sent) {
                    sent = _bm.send(_playbackId, read_buf, sizeof(AudioChunkHeader) + read_bytes, pdMS_TO_TICKS(100));
                }
            } else {
                // EOF reached
                ESP_LOGI(TAG, "Reader Task: Local File EOF reached");
                header->type = ChunkType::EOF_STREAM;
                header->size = 0;
                _bm.send(_playbackId, read_buf, sizeof(AudioChunkHeader), portMAX_DELAY);
                break;
            }
        }
    }

    if (tempFile) {
        fclose(tempFile);
    }
    heap_caps_free(read_buf);

    ESP_LOGI(TAG, "Reader Task exiting");
    _readerTaskRunning = false;
    _readerTaskHandle = nullptr;
    vTaskDelete(NULL);
}

// ─────────────────────────────────────────────────────────────────────────────
// Prefetch Path — writer-only (no reader, no AudioEngine involvement)
// ─────────────────────────────────────────────────────────────────────────────

bool StorageManager::beginPrefetch(const char* songId) {
    if (!songId || songId[0] == '\0') return false;

    // Stop any previous prefetch before starting a new one
    stopPrefetch();

    strncpy(_prefetchSongId, songId, sizeof(_prefetchSongId) - 1);
    _prefetchSongId[sizeof(_prefetchSongId) - 1] = '\0';
    _prefetchComplete = false;
    _prefetchWriterRunning = true;

    ESP_LOGI(TAG, "Starting prefetch writer for songId: %s", songId);

    BaseType_t ret = xTaskCreatePinnedToCore(
        sdPrefetchWriterTaskThunk, "sd_prefetch_task", 4096, this,
        3, &_prefetchWriterHandle, 1  // Lower priority than main writer (4)
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn sd_prefetch_task");
        _prefetchWriterRunning = false;
        _prefetchSongId[0] = '\0';
        return false;
    }

    return true;
}

void StorageManager::stopPrefetch() {
    if (_prefetchWriterRunning || _prefetchWriterHandle != nullptr) {
        ESP_LOGI(TAG, "Stopping prefetch writer...");
        _prefetchWriterRunning = false;

        // Wait for the prefetch task to exit
        while (_prefetchWriterHandle != nullptr) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        ESP_LOGI(TAG, "Prefetch writer stopped");
    }

    // Clean up incomplete prefetch temp file
    if (_prefetchSongId[0] != '\0' && !_prefetchComplete) {
        char tempPath[128];
        snprintf(tempPath, sizeof(tempPath), "/sdcard/music/%s.ogg.tmp", _prefetchSongId);
        if (_storageService.fileExists(tempPath)) {
            ESP_LOGI(TAG, "Cleaning up incomplete prefetch temp: %s", tempPath);
            _storageService.deleteFile(tempPath);
        }
    }

    _prefetchComplete = false;
    _prefetchSongId[0] = '\0';
}

void StorageManager::sdPrefetchWriterTaskThunk(void* pvParameters) {
    static_cast<StorageManager*>(pvParameters)->runPrefetchWriterLoop();
}

void StorageManager::runPrefetchWriterLoop() {
    ESP_LOGI(TAG, "Prefetch Writer Task running on Core 1 for songId: %s", _prefetchSongId);

    // Ensure music directory exists
    struct stat st;
    if (stat("/sdcard/music", &st) != 0) {
        mkdir("/sdcard/music", 0755);
    }

    char tempPath[128];
    char targetPath[128];
    snprintf(tempPath,   sizeof(tempPath),   "/sdcard/music/%s.ogg.tmp", _prefetchSongId);
    snprintf(targetPath, sizeof(targetPath), "/sdcard/music/%s.ogg",     _prefetchSongId);

    FILE* prefetchStream = _storageService.openStream(tempPath, "wb");
    if (!prefetchStream) {
        ESP_LOGE(TAG, "Prefetch Writer: Failed to open temp file: %s", tempPath);
        _prefetchWriterRunning = false;
        _prefetchWriterHandle = nullptr;
        vTaskDelete(NULL);
        return;
    }

    // Allocate write buffer in PSRAM
    size_t allocSize = sizeof(AudioChunkHeader) + AUDIO_CHUNK_SIZE;
    uint8_t* write_buf = static_cast<uint8_t*>(heap_caps_malloc(allocSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!write_buf) {
        ESP_LOGE(TAG, "Prefetch Writer: Failed to allocate buffer in PSRAM");
        _storageService.closeStream(prefetchStream);
        _prefetchWriterRunning = false;
        _prefetchWriterHandle = nullptr;
        vTaskDelete(NULL);
        return;
    }

    bool success = false;

    while (_prefetchWriterRunning) {
        size_t rx_bytes = 0;
        void* rx_ptr = _bm.receive(_storageId, &rx_bytes, pdMS_TO_TICKS(200));
        if (rx_ptr == nullptr) {
            continue;
        }

        AudioChunkHeader* chunk = reinterpret_cast<AudioChunkHeader*>(rx_ptr);

        if (chunk->type == ChunkType::EOF_STREAM) {
            ESP_LOGI(TAG, "Prefetch Writer: Received EOF — prefetch download complete");
            success = true;
            _bm.returnItem(_storageId, rx_ptr);
            break;
        } else if (chunk->type == ChunkType::ERROR) {
            ESP_LOGE(TAG, "Prefetch Writer: Received ERROR signal");
            _bm.returnItem(_storageId, rx_ptr);
            break;
        }

        if (chunk->type == ChunkType::DATA && chunk->size > 0) {
            uint8_t* payload = reinterpret_cast<uint8_t*>(chunk) + sizeof(AudioChunkHeader);
            size_t written = _storageService.writeStream(prefetchStream, payload, chunk->size);
            if (written != chunk->size) {
                ESP_LOGE(TAG, "Prefetch Writer: Disk write error! Expected %u, wrote %u",
                         (unsigned)chunk->size, (unsigned)written);
            } else {
                fflush(prefetchStream);
                fsync(fileno(prefetchStream));
            }
        }

        _bm.returnItem(_storageId, rx_ptr);
    }

    _storageService.closeStream(prefetchStream);
    heap_caps_free(write_buf);

    if (success) {
        // Commit: rename .ogg.tmp → .ogg
        if (_storageService.fileExists(targetPath)) {
            _storageService.deleteFile(targetPath);
        }
        int ret = rename(tempPath, targetPath);
        if (ret == 0) {
            ESP_LOGI(TAG, "Prefetch Writer: Successfully cached next song: %s", targetPath);
            _prefetchComplete = true;
        } else {
            ESP_LOGE(TAG, "Prefetch Writer: Failed to rename temp file (errno %d)", errno);
        }
    } else {
        // Cleanup incomplete temp file
        if (_storageService.fileExists(tempPath)) {
            _storageService.deleteFile(tempPath);
        }
    }

    ESP_LOGI(TAG, "Prefetch Writer Task exiting");
    _prefetchWriterRunning = false;
    _prefetchWriterHandle = nullptr;
    vTaskDelete(NULL);
}

