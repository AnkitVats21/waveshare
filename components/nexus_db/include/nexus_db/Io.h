#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace nexus_db {

// One open database file. All offsets are absolute.
class LogFile {
public:
    virtual ~LogFile() = default;
    // True only if exactly `len` bytes were read.
    virtual bool readAt(uint32_t offset, void* dst, size_t len) = 0;
    // Writes at the end of the file. True only if all bytes were written.
    virtual bool append(const void* src, size_t len) = 0;
    virtual long size() = 0;
    virtual bool sync() = 0;
    virtual bool truncate(uint32_t length) = 0;
};

// The file operations the engine needs. The device uses sd_storage (SdIo);
// host tests use POSIX files and inject faults.
class Io {
public:
    virtual ~Io() = default;
    // Read-write, without truncating. `create` makes a missing file.
    virtual std::unique_ptr<LogFile> open(const char* path, bool create) = 0;
    // Read-write, created or truncated to zero.
    virtual std::unique_ptr<LogFile> create(const char* path) = 0;
    virtual bool exists(const char* path) = 0;
    virtual bool remove(const char* path) = 0;
    // Fails if `to` exists.
    virtual bool rename(const char* from, const char* to) = 0;
    virtual bool mkdirs(const char* dir) = 0;
};

#ifdef ESP_PLATFORM
// sd_storage-backed Io for files on the SD card.
Io& sdIo();
#endif

} // namespace nexus_db
