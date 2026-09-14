#pragma once

#include <cstdio>
#include <cstddef>
#include <string>

/**
 * @brief Abstract storage interface for file and stream operations.
 * Decouples core and application components from the board-level StorageService.
 */
class IStorageService {
public:
    virtual ~IStorageService() = default;

    virtual bool isMounted() const = 0;
    virtual bool fileExists(const char* path) = 0;
    virtual bool deleteFile(const char* path) = 0;
    virtual bool writeFile(const char* path, const char* content) = 0;
    virtual bool appendFile(const char* path, const char* content) = 0;
    virtual std::string readFile(const char* path) = 0;

    virtual FILE* openStream(const char* path, const char* mode) = 0;
    virtual size_t writeStream(FILE* stream, const void* buffer, size_t size) = 0;
    virtual size_t readStream(FILE* stream, void* buffer, size_t size) = 0;
    virtual void closeStream(FILE* stream) = 0;
};
