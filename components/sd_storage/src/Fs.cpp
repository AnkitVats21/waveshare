#include "sd_storage/Fs.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include "esp_log.h"
#include "sd_storage/File.h"
#include "sd_storage/SdCard.h"
#include "OpenFileTable.h"

namespace sd_storage {
namespace Fs {

namespace {
const char* TAG = "sd_storage";

constexpr size_t kMaxPath = 320;

bool tmpPathFor(const char* path, char* out) {
    return snprintf(out, kMaxPath, "%s.tmp", path) < static_cast<int>(kMaxPath);
}

bool isMountRoot(const char* path) {
    return strcmp(path, SdCard::instance().mountPoint()) == 0;
}

// A writeAtomic that lost power between removing the old file and renaming
// the new one leaves only <path>.tmp, fully written and synced. Finish it.
void finishInterruptedSwap(const char* path) {
    struct stat st;
    if (::stat(path, &st) == 0) return;
    char tmp[kMaxPath];
    if (!tmpPathFor(path, tmp) || ::stat(tmp, &st) != 0) return;
    if (::rename(tmp, path) == 0) {
        ESP_LOGW(TAG, "recovered %s from an interrupted write", path);
    }
}

// Holds a path as if open for writing, so nothing reads or writes it while
// it is removed, renamed or swapped. A null path holds nothing.
class PathGuard {
public:
    explicit PathGuard(const char* path)
        : m_key(path ? OpenFileTable::key(path) : 0),
          m_acquired(path && OpenFileTable::acquire(m_key, true, false, File::kDefaultWait)),
          m_held(!path || m_acquired) {}
    ~PathGuard() {
        if (m_acquired) OpenFileTable::release(m_key, true, false);
    }
    bool held() const { return m_held; }
private:
    uint32_t m_key;
    bool m_acquired;
    bool m_held;
};
} // namespace

std::string readText(const char* path, size_t max_bytes) {
    std::string out;
    if (!SdCard::instance().isMounted() || path == nullptr) return out;
    finishInterruptedSwap(path);
    File f = File::open(path, Mode::Read);
    if (!f) return out;
    long size = f.size();
    if (size <= 0) return out;
    if (static_cast<size_t>(size) > max_bytes) {
        ESP_LOGW(TAG, "%s is %ld bytes, over the %u limit", path, size, (unsigned)max_bytes);
        return out;
    }
    out.resize(static_cast<size_t>(size));
    out.resize(f.read(&out[0], out.size()));
    return out;
}

size_t readInto(const char* path, void* dst, size_t len) {
    if (!SdCard::instance().isMounted() || path == nullptr) return 0;
    finishInterruptedSwap(path);
    File f = File::open(path, Mode::Read);
    return f ? f.read(dst, len) : 0;
}

bool writeAtomic(const char* path, const void* data, size_t len) {
    if (!SdCard::instance().isMounted() || path == nullptr) return false;
    char tmp[kMaxPath];
    if (!tmpPathFor(path, tmp)) return false;

    {
        File f = File::open(tmp, Mode::Write);
        if (!f) return false;
        if ((len > 0 && !f.writeAll(data, len)) || !f.sync()) {
            f.close();
            ::unlink(tmp);
            ESP_LOGE(TAG, "write %s failed", path);
            return false;
        }
    }

    PathGuard guard(path);
    if (!guard.held()) {
        ::unlink(tmp);
        return false;
    }
    // FATFS rename refuses an existing target, so the old file goes first.
    if (::unlink(path) != 0 && errno != ENOENT) {
        ESP_LOGE(TAG, "replace %s failed (errno %d)", path, errno);
        ::unlink(tmp);
        return false;
    }
    if (::rename(tmp, path) != 0) {
        ESP_LOGE(TAG, "rename to %s failed (errno %d)", path, errno);
        return false;
    }
    return true;
}

bool writeAtomic(const char* path, const char* text) {
    return writeAtomic(path, text, text ? strlen(text) : 0);
}

bool append(const char* path, const void* data, size_t len) {
    if (!SdCard::instance().isMounted() || path == nullptr) return false;
    File f = File::open(path, Mode::Append);
    return f && f.writeAll(data, len);
}

bool stat(const char* path, PathInfo& out) {
    if (!SdCard::instance().isMounted() || path == nullptr) return false;
    if (isMountRoot(path)) {
        out = PathInfo{true, 0, 0};
        return true;
    }
    struct stat st;
    if (::stat(path, &st) != 0) return false;
    out.is_dir = S_ISDIR(st.st_mode);
    out.size = out.is_dir ? 0 : static_cast<size_t>(st.st_size);
    out.mtime = st.st_mtime;
    return true;
}

bool exists(const char* path) {
    PathInfo info;
    return stat(path, info);
}

bool isFile(const char* path) {
    PathInfo info;
    return stat(path, info) && !info.is_dir;
}

bool isDir(const char* path) {
    PathInfo info;
    return stat(path, info) && info.is_dir;
}

bool remove(const char* path) {
    if (!isFile(path)) return false;
    PathGuard guard(path);
    if (!guard.held()) return false;
    if (::unlink(path) != 0) {
        ESP_LOGE(TAG, "remove %s failed (errno %d)", path, errno);
        return false;
    }
    return true;
}

bool removePath(const char* path) {
    PathInfo info;
    if (!stat(path, info) || isMountRoot(path)) return false;
    if (!info.is_dir) return remove(path);
    if (::rmdir(path) != 0) {
        ESP_LOGE(TAG, "rmdir %s failed (errno %d)", path, errno);
        return false;
    }
    return true;
}

bool rename(const char* old_path, const char* new_path) {
    if (!exists(old_path) || new_path == nullptr) return false;
    PathGuard from(old_path);
    // A case-only rename is the same FAT entry, so it needs only one guard.
    PathGuard to(strcasecmp(old_path, new_path) == 0 ? nullptr : new_path);
    if (!from.held() || !to.held()) return false;
    if (::rename(old_path, new_path) != 0) {
        ESP_LOGE(TAG, "rename %s -> %s failed (errno %d)", old_path, new_path, errno);
        return false;
    }
    return true;
}

bool mkdirs(const char* dir_path) {
    if (!SdCard::instance().isMounted() || dir_path == nullptr) return false;
    char buf[kMaxPath];
    size_t len = strlen(dir_path);
    if (len == 0 || len >= sizeof(buf)) return false;
    memcpy(buf, dir_path, len + 1);
    while (len > 1 && buf[len - 1] == '/') buf[--len] = '\0';

    // Every prefix below the mount point, then the full path.
    size_t start = strlen(SdCard::instance().mountPoint()) + 1;
    for (size_t i = start; i <= len; ++i) {
        if (buf[i] != '/' && buf[i] != '\0') continue;
        char saved = buf[i];
        buf[i] = '\0';
        if (::mkdir(buf, 0755) != 0 && errno != EEXIST) {
            ESP_LOGE(TAG, "mkdir %s failed (errno %d)", buf, errno);
            return false;
        }
        buf[i] = saved;
    }
    return true;
}

bool list(const char* dir_path, const char* ext, bool with_stat, ListCallback cb, void* ctx) {
    if (!SdCard::instance().isMounted() || dir_path == nullptr || cb == nullptr) return false;
    DIR* dir = opendir(dir_path);
    if (dir == nullptr) {
        ESP_LOGW(TAG, "opendir %s failed", dir_path);
        return false;
    }

    char full[kMaxPath];
    size_t dir_len = strlen(dir_path);
    bool slash = dir_len > 0 && dir_path[dir_len - 1] == '/';

    struct dirent* e;
    while ((e = readdir(dir)) != nullptr) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        DirEntry entry{e->d_name, e->d_type == DT_DIR, 0, 0};
        if (ext != nullptr) {
            const char* dot = strrchr(e->d_name, '.');
            if (entry.is_dir || dot == nullptr || strcasecmp(dot, ext) != 0) continue;
        }
        if (with_stat &&
            snprintf(full, sizeof(full), slash ? "%s%s" : "%s/%s", dir_path, e->d_name) < static_cast<int>(sizeof(full))) {
            struct stat st;
            if (::stat(full, &st) == 0) {
                entry.is_dir = S_ISDIR(st.st_mode);
                entry.size = entry.is_dir ? 0 : static_cast<size_t>(st.st_size);
                entry.mtime = st.st_mtime;
            }
        }
        if (!cb(entry, ctx)) break;
    }
    closedir(dir);
    return true;
}

bool info(uint64_t& total_bytes, uint64_t& free_bytes) {
    return SdCard::instance().info(total_bytes, free_bytes);
}

} // namespace Fs
} // namespace sd_storage
