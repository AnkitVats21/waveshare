#include "sd_storage/File.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <utility>
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "OpenFileTable.h"

namespace sd_storage {

namespace {
const char* TAG = "sd_storage";

// FATFS hands the caller's buffer straight to the SDMMC driver when a
// transfer covers whole sectors, and the driver only bounces PSRAM. A buffer
// in RTC RAM or flash would be DMA'd to silently and wrongly. Transfers
// smaller than the smallest sector always go through FATFS's own sector
// cache, so such buffers are moved in pieces of this size.
constexpr size_t kUnsafePiece = 256;

bool dmaUnsafe(const void* p) {
    return !esp_ptr_external_ram(p) && !esp_ptr_dma_capable(p);
}

int openFlags(Mode mode) {
    switch (mode) {
        case Mode::Read:           return O_RDONLY;
        case Mode::Write:          return O_WRONLY | O_CREAT | O_TRUNC;
        case Mode::Append:         return O_WRONLY | O_CREAT | O_APPEND;
        case Mode::Update:         return O_RDWR;
        case Mode::UpdateOrCreate: return O_RDWR | O_CREAT;
    }
    return O_RDONLY;
}
} // namespace

File::File(File&& other) noexcept
    : m_fd(std::exchange(other.m_fd, -1)),
      m_key(other.m_key),
      m_writer(other.m_writer),
      m_follower(other.m_follower) {}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        close();
        m_fd = std::exchange(other.m_fd, -1);
        m_key = other.m_key;
        m_writer = other.m_writer;
        m_follower = other.m_follower;
    }
    return *this;
}

File File::open(const char* path, Mode mode, Share share, TickType_t wait) {
    if (path == nullptr) return File();
    bool writer = mode != Mode::Read;
    bool follower = !writer && share == Share::FollowWriter;
    uint32_t k = OpenFileTable::key(path);
    if (!OpenFileTable::acquire(k, writer, follower, wait)) {
        ESP_LOGW(TAG, "busy: %s", path);
        return File();
    }
    int fd = ::open(path, openFlags(mode), 0644);
    if (fd < 0) {
        int err = errno;
        OpenFileTable::release(k, writer, follower);
        if (mode != Mode::Read || err != ENOENT) {
            ESP_LOGW(TAG, "open %s failed (errno %d)", path, err);
        }
        return File();
    }
    return File(fd, k, writer, follower);
}

size_t File::read(void* dst, size_t len) {
    if (m_fd < 0 || dst == nullptr) return 0;
    uint8_t* out = static_cast<uint8_t*>(dst);
    size_t piece_max = dmaUnsafe(dst) ? kUnsafePiece : len;
    size_t done = 0;
    while (done < len) {
        size_t want = len - done < piece_max ? len - done : piece_max;
        ssize_t n = ::read(m_fd, out + done, want);
        if (n <= 0) break;
        done += static_cast<size_t>(n);
        if (static_cast<size_t>(n) < want) break;
    }
    return done;
}

size_t File::write(const void* src, size_t len) {
    if (m_fd < 0 || src == nullptr) return 0;
    const uint8_t* in = static_cast<const uint8_t*>(src);
    size_t piece_max = dmaUnsafe(src) ? kUnsafePiece : len;
    size_t done = 0;
    while (done < len) {
        size_t want = len - done < piece_max ? len - done : piece_max;
        ssize_t n = ::write(m_fd, in + done, want);
        if (n <= 0) {
            ESP_LOGW(TAG, "write failed (errno %d)", errno);
            break;
        }
        done += static_cast<size_t>(n);
    }
    return done;
}

bool File::seek(long offset, int whence) {
    return m_fd >= 0 && lseek(m_fd, offset, whence) >= 0;
}

long File::tell() const {
    return m_fd >= 0 ? static_cast<long>(lseek(m_fd, 0, SEEK_CUR)) : -1;
}

long File::size() const {
    struct stat st;
    if (m_fd < 0 || fstat(m_fd, &st) != 0) return -1;
    return static_cast<long>(st.st_size);
}

bool File::sync() {
    return m_fd >= 0 && fsync(m_fd) == 0;
}

bool File::truncate(long length) {
    return m_fd >= 0 && ftruncate(m_fd, length) == 0;
}

void File::close() {
    if (m_fd < 0) return;
    ::close(m_fd);
    m_fd = -1;
    OpenFileTable::release(m_key, m_writer, m_follower);
}

} // namespace sd_storage
