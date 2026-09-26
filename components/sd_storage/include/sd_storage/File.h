#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include "freertos/FreeRTOS.h"

namespace sd_storage {

enum class Mode {
    Read,           // existing file, read only
    Write,          // create or truncate, write only
    Append,         // create if missing, writes go to the end
    Update,         // existing file, read and write
    UpdateOrCreate, // create if missing, read and write, no truncate
};

enum class Share {
    Exclusive,    // readers never overlap a writer
    FollowWriter, // read a file while another handle is still writing it
                  // (the music cache reader tails the .tmp being downloaded)
};

// RAII handle over a POSIX fd on the SD card. No stdio: FATFS already keeps a
// sector cache per open file, so there is no hidden newlib buffer and no
// extra copy.
//
// Opens are checked against every other File open on the same path: a second
// writer, or a reader against a writer, waits up to `wait` and then fails.
//
// Buffers may live anywhere. A buffer the SDMMC DMA can't reach (RTC RAM,
// flash) is copied through PSRAM instead of being handed to the driver, which
// would otherwise transfer to it silently and wrongly.
class File {
public:
    static constexpr TickType_t kDefaultWait = pdMS_TO_TICKS(2000);

    File() = default;
    ~File() { close(); }
    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    static File open(const char* path, Mode mode, Share share = Share::Exclusive,
                     TickType_t wait = kDefaultWait);

    bool isOpen() const { return m_fd >= 0; }
    explicit operator bool() const { return isOpen(); }

    // Returns bytes read; 0 at end of file or on error.
    size_t read(void* dst, size_t len);
    // True only if exactly `len` bytes were read.
    bool readExact(void* dst, size_t len) { return read(dst, len) == len; }

    // Returns bytes written.
    size_t write(const void* src, size_t len);
    bool writeAll(const void* src, size_t len) { return write(src, len) == len; }

    bool seek(long offset, int whence = SEEK_SET);
    long tell() const;
    long size() const;
    bool sync();
    bool truncate(long length);
    void close();

private:
    File(int fd, uint32_t key, bool writer, bool follower)
        : m_fd(fd), m_key(key), m_writer(writer), m_follower(follower) {}

    int m_fd = -1;
    uint32_t m_key = 0;
    bool m_writer = false;
    bool m_follower = false;
};

} // namespace sd_storage
