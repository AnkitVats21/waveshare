#pragma once

#include <string>
#include <vector>
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace Services {

class StorageService {
public:
    static StorageService& getInstance();

    // Check if the filesystem is mounted (bound to Board's SdCardManager)
    bool isMounted() const;

    // File operation APIs (thread-safe via internal mutex)
    bool writeFile(const char* path, const char* content);
    bool appendFile(const char* path, const char* content);
    std::string readFile(const char* path);
    bool deleteFile(const char* path);
    bool fileExists(const char* path);

    // Callback-based directory listing to prevent heap fragmentation
    typedef void (*FileFoundCallback)(const char* filename, void* ctx);
    void listFiles(const char* dir_path, const char* extension, FileFoundCallback cb, void* ctx);

    // Opens a continuous binary handle (returns a raw pointer or file descriptor wrapper)
    FILE* openStream(const char* path, const char* mode);
    // Writes raw binary array blocks securely without string truncation
    size_t writeStream(FILE* stream, const void* buffer, size_t size);
    // Reads raw binary array blocks securely
    size_t readStream(FILE* stream, void* buffer, size_t size);
    // Checks if the active continuous stream reached the final file marker
    bool isStreamEOF(FILE* stream);
    // Flushes blocks to flash media and securely closes the stream
    void closeStream(FILE* stream);

private:
    StorageService();
    ~StorageService();
    StorageService(const StorageService&) = delete;
    StorageService& operator=(const StorageService&) = delete;

    SemaphoreHandle_t m_file_mutex = nullptr;
};

} // namespace Services
