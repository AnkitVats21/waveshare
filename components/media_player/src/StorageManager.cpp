#include "StorageManager.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "sd_storage/Fs.h"
#include "sd_storage/SdCard.h"
#include <cstring>
#include <algorithm>
#include "PlayerTypes.h"
#include "media_player/CatalogDB.h"
#include "common/thread_config.h"
#include "freertos/idf_additions.h"

static const char* TAG = "StorageManager";

StorageManager::StorageManager(BufferManager::BufferId playbackId, BufferManager::BufferId storageId)
    : _bm(BufferManager::getInstance()),
      _playbackId(playbackId),
      _storageId(storageId) {
    _streamMutex = xSemaphoreCreateMutex();
}

StorageManager::~StorageManager() {
    closeActiveFile();
    if (_streamMutex) {
        vSemaphoreDelete(_streamMutex);
        _streamMutex = nullptr;
    }
}

bool StorageManager::getValidCachedPath(const char* songId, char* outPath, size_t maxLen) {
    if (!songId || songId[0] == '\0' || !outPath || maxLen == 0) return false;
    const char* extensions[] = {".webm", ".opus", ".ogg"};
    sd_storage::PathInfo info;

    for (const char* ext : extensions) {
        snprintf(outPath, maxLen, "/sdcard/music/%s%s", songId, ext);
        if (sd_storage::Fs::stat(outPath, info)) {
            if (info.size >= 32768) {
                return true;
            }
            ESP_LOGW(TAG, "Cached file %s is corrupt or incomplete (size=%ld < 32KB). Deleting.", outPath, (long)info.size);
            sd_storage::Fs::remove(outPath);
        }
    }
    outPath[0] = '\0';
    return false;
}

bool StorageManager::fileExists(const char* songId) {
    char path[128];
    return getValidCachedPath(songId, path, sizeof(path));
}

bool StorageManager::deleteFile(const char* songId) {
    if (!songId || songId[0] == '\0') return false;
    char path[128];
    bool deleted = false;

    for (const char* ext : {".webm", ".opus", ".ogg"}) {
        snprintf(path, sizeof(path), "/sdcard/music/%s%s", songId, ext);
        if (sd_storage::Fs::isFile(path)) {
            deleted = sd_storage::Fs::remove(path) || deleted;
        }
    }
    if (deleted) {
        ESP_LOGI(TAG, "Deleted local cached audio file(s) for songId: %s", songId);
        CatalogDB::getInstance().setSaved(songId, 0);
    }
    return deleted;
}

bool StorageManager::openFileForCaching(const char* songId, size_t expectedBytes) {
    if (!songId || !sd_storage::SdCard::instance().isMounted()) return false;
    closeActiveFile();

    if (!sd_storage::Fs::mkdirs("/sdcard/music")) return false;

    char tempPath[128];
    snprintf(tempPath, sizeof(tempPath), "/sdcard/music/%s.webm.tmp", songId);

    ESP_LOGI(TAG, "Opening cache stream at: %s", tempPath);
    _writeFile = sd_storage::File::open(tempPath, sd_storage::Mode::Write);
    if (!_writeFile) {
        ESP_LOGE(TAG, "Failed to open cache stream for writing: %s", tempPath);
        return false;
    }

    strncpy(_currentSongId, songId, sizeof(_currentSongId) - 1);
    _currentSongId[sizeof(_currentSongId) - 1] = '\0';
    _downloadComplete = false;
    _bytesWritten = 0;
    _expectedBytes = expectedBytes;
    _isWritingMode = true;

    _writerTaskRunning = true;
    _readerTaskRunning = true;

    // Spawn concurrent SD Writer task on Core 0 (off audio DSP core)
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        sdWriterTaskThunk, "sd_writer_task", ThreadConfig::StackSize::STACK_STORAGE, this,
        ThreadConfig::Priority::STORAGE_IO, &_writerTaskHandle, ThreadConfig::CORE_STORAGE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn sd_writer_task");
        closeActiveFile();
        return false;
    }

    // Spawn concurrent SD Reader task on Core 0 (off audio DSP core)
    ret = xTaskCreatePinnedToCoreWithCaps(
        sdReaderTaskThunk, "sd_reader_task", ThreadConfig::StackSize::STACK_STORAGE, this,
        ThreadConfig::Priority::STORAGE_IO, &_readerTaskHandle, ThreadConfig::CORE_STORAGE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn sd_reader_task");
        closeActiveFile();
        return false;
    }

    return true;
}

bool StorageManager::openFileForReading(const char* songId) {
    if (!songId || !sd_storage::SdCard::instance().isMounted()) return false;
    char path[128];
    if (!getValidCachedPath(songId, path, sizeof(path))) {
        ESP_LOGE(TAG, "No valid local audio file found for songId: %s", songId);
        return false;
    }
    if (!openPathForReading(path)) return false;
    strncpy(_currentSongId, songId, sizeof(_currentSongId) - 1);
    _currentSongId[sizeof(_currentSongId) - 1] = '\0';
    return true;
}

bool StorageManager::openPathForReading(const char* path) {
    if (!path || !sd_storage::SdCard::instance().isMounted()) return false;
    closeActiveFile();

    _readFile = sd_storage::File::open(path, sd_storage::Mode::Read);
    ESP_LOGI(TAG, "Opening local playback stream at: %s (size=%ld bytes)", path, _readFile.size());
    if (!_readFile) {
        ESP_LOGE(TAG, "Failed to open playback stream: %s", path);
        return false;
    }

    _currentSongId[0] = '\0';
    _downloadComplete = true; // Local playback is already complete
    _isWritingMode = false;
    _readerAtEof = false;

    if (!spawnReader()) {
        closeActiveFile();
        return false;
    }
    return true;
}

bool StorageManager::spawnReader() {
    _readerTaskRunning = true;
    // Spawn concurrent SD Reader task on Core 0 (off audio DSP core)
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        sdReaderTaskThunk, "sd_reader_task", ThreadConfig::StackSize::STACK_STORAGE, this,
        ThreadConfig::Priority::STORAGE_IO, &_readerTaskHandle, ThreadConfig::CORE_STORAGE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn sd_reader_task");
        _readerTaskRunning = false;
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
    _writeFile.close();
    _readFile.close();

    // If caching, finalize or clean up
    if (_isWritingMode && _currentSongId[0] != '\0') {
        char tempPath[128];
        char targetPath[128];
        snprintf(tempPath, sizeof(tempPath), "/sdcard/music/%s.webm.tmp", _currentSongId);
        snprintf(targetPath, sizeof(targetPath), "/sdcard/music/%s.webm", _currentSongId);

        // EOF also arrives when the server closes early; trust it only if the
        // byte count matches the length the stream URL advertised.
        const bool truncated = _expectedBytes > 0 && _bytesWritten != _expectedBytes;
        if (_downloadComplete && truncated) {
            ESP_LOGW(TAG, "Download ended at %u of %u bytes; discarding %s",
                     (unsigned)_bytesWritten, (unsigned)_expectedBytes, tempPath);
            sd_storage::Fs::remove(tempPath);
        } else if (_downloadComplete) {
            ESP_LOGI(TAG, "Download complete. Committing cache to target: %s", targetPath);
            if (sd_storage::Fs::isFile(targetPath)) {
                sd_storage::Fs::remove(targetPath);
            }
            if (!sd_storage::Fs::rename(tempPath, targetPath)) {
                ESP_LOGE(TAG, "Failed to commit cached file %s", targetPath);
            } else {
                ESP_LOGI(TAG, "Successfully committed cache file: %s", targetPath);
                CatalogDB::getInstance().setSaved(_currentSongId, static_cast<uint32_t>(_bytesWritten));
            }
        } else {
            ESP_LOGI(TAG, "Download incomplete or aborted. Cleaning up temp cache: %s", tempPath);
            if (sd_storage::Fs::isFile(tempPath)) {
                sd_storage::Fs::remove(tempPath);
            }
        }
    }

    _isWritingMode = false;
    _readerAtEof = false;
    _downloadComplete = false;
    _bytesWritten = 0;
    _expectedBytes = 0;
    _currentSongId[0] = '\0';
}

bool StorageManager::seekTo(uint32_t byteOffset) {
    if (!_readFile) return false;
    if (_streamMutex) xSemaphoreTake(_streamMutex, portMAX_DELAY);

    ESP_LOGI(TAG, "Seeking local stream to byte offset: %u", (unsigned int)byteOffset);
    _readFile.seek(byteOffset);
    _readGen++;
    // Flush any pending data in playback buffer to avoid playing stale audio
    _bm.flush(_playbackId);
    const bool restart = _readerAtEof;
    _readerAtEof = false;

    if (_streamMutex) xSemaphoreGive(_streamMutex);

    if (restart) {
        // The reader sent EOF and is exiting; start a new one at the new position.
        for (int i = 0; i < 50 && _readerTaskHandle != nullptr; ++i) vTaskDelay(pdMS_TO_TICKS(10));
        if (_readerTaskHandle != nullptr) {
            ESP_LOGW(TAG, "Reader did not exit after EOF; seek not restarted");
            return false;
        }
        return spawnReader();
    }
    return true;
}

size_t StorageManager::readHead(uint8_t* dst, size_t len) {
    if (!_readFile || !dst) return 0;
    if (_streamMutex) xSemaphoreTake(_streamMutex, portMAX_DELAY);
    const long pos = _readFile.tell();
    size_t got = 0;
    if (pos >= 0 && _readFile.seek(0)) {
        got = _readFile.read(dst, len);
        _readFile.seek(pos);
    }
    if (_streamMutex) xSemaphoreGive(_streamMutex);
    return got;
}

size_t StorageManager::fileSize() {
    if (!_readFile) return 0;
    const long size = _readFile.size();
    return size > 0 ? size_t(size) : 0;
}

StorageManager::Send StorageManager::sendUnlessMoved(const uint8_t* buf, size_t len, uint32_t gen) {
    // Holding the lock across the send keeps a seek from flushing the ring
    // between the check and the send; the short timeout keeps seekTo waiting
    // at most ~20 ms when the ring is full.
    while (_readerTaskRunning) {
        if (_streamMutex) xSemaphoreTake(_streamMutex, portMAX_DELAY);
        if (gen != _readGen) {
            if (_streamMutex) xSemaphoreGive(_streamMutex);
            return Send::Moved;
        }
        const bool sent = _bm.send(_playbackId, buf, len, pdMS_TO_TICKS(20));
        if (sent && reinterpret_cast<const AudioChunkHeader*>(buf)->type == ChunkType::EOF_STREAM) {
            _readerAtEof = true;
        }
        if (_streamMutex) xSemaphoreGive(_streamMutex);
        if (sent) return Send::Sent;
    }
    return Send::Stopped;
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
            ESP_LOGI(TAG, "Writer Task: download aborted (stop or stream error)");
            _bm.returnItem(_storageId, rx_ptr);
            break;
        }

        if (chunk->type == ChunkType::DATA && chunk->size > 0) {
            uint8_t* payload = reinterpret_cast<uint8_t*>(chunk) + sizeof(AudioChunkHeader);
            size_t written = _writeFile.write(payload, chunk->size);
            if (written != chunk->size) {
                ESP_LOGE(TAG, "Writer Task: Disk write error! Expected %u, wrote %u", (unsigned)chunk->size, (unsigned)written);
            } else {
                // Sync so the reader's own handle sees the new length.
                _writeFile.sync();
                _bytesWritten += written;
            }
        }

        _bm.returnItem(_storageId, rx_ptr);
    }

    ESP_LOGI(TAG, "Writer Task exiting");
    _writerTaskRunning = false;
    _writerTaskHandle = nullptr;
    vTaskDeleteWithCaps(nullptr);  // created WithCaps: plain vTaskDelete leaks the stack
}

void StorageManager::runReaderTaskLoop() {
    ESP_LOGI(TAG, "Reader Task running on Core %d", (int)xPortGetCoreID());

    // Pre-allocate read buffer in PSRAM to prevent stack overflows (32KB payload)
    size_t allocSize = sizeof(AudioChunkHeader) + AUDIO_CHUNK_SIZE;
    uint8_t* read_buf = static_cast<uint8_t*>(heap_caps_malloc(allocSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!read_buf) {
        ESP_LOGE(TAG, "Reader Task: Failed to allocate read buffer in PSRAM!");
        _readerTaskRunning = false;
        _readerTaskHandle = nullptr;
        vTaskDeleteWithCaps(nullptr);  // created WithCaps: plain vTaskDelete leaks the stack
        return;
    }

    AudioChunkHeader* header = reinterpret_cast<AudioChunkHeader*>(read_buf);
    uint8_t* payload = read_buf + sizeof(AudioChunkHeader);

    size_t read_pos = 0;
    sd_storage::File tempFile;

    char tempPath[128];
    if (_isWritingMode) {
        snprintf(tempPath, sizeof(tempPath), "/sdcard/music/%s.webm.tmp", _currentSongId);
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
                    tempFile.close();
                    vTaskDelay(pdMS_TO_TICKS(100));
                    continue;
                }
            }

            // Open/reopen the temp file if not currently open
            if (!tempFile) {
                tempFile = sd_storage::File::open(tempPath, sd_storage::Mode::Read, sd_storage::Share::FollowWriter);
                if (!tempFile) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                    continue;
                }
                tempFile.seek(read_pos);
            }

            // Read the next chunk up to the current write threshold
            size_t bytes_to_read = std::min(AUDIO_CHUNK_SIZE, _bytesWritten - read_pos);
            if (bytes_to_read > 0) {
                size_t read_bytes = tempFile.read(payload, bytes_to_read);
                if (read_bytes > 0) {
                    header->type = ChunkType::DATA;
                    header->size = read_bytes;
                    // Blocks if PLAYER_BUF is full, regulating progressive reading
                    if (_bm.send(_playbackId, read_buf, sizeof(AudioChunkHeader) + read_bytes, pdMS_TO_TICKS(100))) {
                        read_pos += read_bytes;
                    } else {
                        // Reseek to retry sending
                        tempFile.seek(read_pos);
                    }
                } else {
                    // Stale file size read? Wait/reopen
                    tempFile.close();
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        } else {
            // Standard Local Cache Hit Playback
            if (_streamMutex) xSemaphoreTake(_streamMutex, portMAX_DELAY);
            const uint32_t gen = _readGen;
            size_t read_bytes = _readFile.read(payload, AUDIO_CHUNK_SIZE);
            if (_streamMutex) xSemaphoreGive(_streamMutex);
            if (read_bytes > 0) {
                header->type = ChunkType::DATA;
                header->size = read_bytes;
                // Backpressure: waits while the player buffer is full.
                sendUnlessMoved(read_buf, sizeof(AudioChunkHeader) + read_bytes, gen);
            } else {
                header->type = ChunkType::EOF_STREAM;
                header->size = 0;
                // A seek after this read makes the EOF stale: keep reading.
                if (sendUnlessMoved(read_buf, sizeof(AudioChunkHeader), gen) == Send::Sent) {
                    ESP_LOGI(TAG, "Reader Task: Local File EOF reached");
                    break;
                }
            }
        }
    }

    tempFile.close();  // before the task deletes itself: no destructor runs after that
    heap_caps_free(read_buf);

    ESP_LOGI(TAG, "Reader Task exiting");
    _readerTaskRunning = false;
    _readerTaskHandle = nullptr;
    vTaskDeleteWithCaps(nullptr);  // created WithCaps: plain vTaskDelete leaks the stack
}
