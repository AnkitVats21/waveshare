#pragma once

#include <string>
#include <mutex>
#include <vector>

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

private:
    StorageService() = default;
    ~StorageService() = default;
    StorageService(const StorageService&) = delete;
    StorageService& operator=(const StorageService&) = delete;

    std::mutex m_file_mutex;
};

} // namespace Services
