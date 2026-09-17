#include "StorageService.h"
#include "hal/Board.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* TAG = "StorageService";

namespace {
// Thread-safe RAII helper for FreeRTOS mutex
class FreeRTOSLock {
public:
    explicit FreeRTOSLock(SemaphoreHandle_t mutex) : m_mutex(mutex) {
        if (m_mutex) {
            xSemaphoreTake(m_mutex, portMAX_DELAY);
        }
    }
    ~FreeRTOSLock() {
        if (m_mutex) {
            xSemaphoreGive(m_mutex);
        }
    }
private:
    SemaphoreHandle_t m_mutex;
};
}

namespace Services {

StorageService& StorageService::getInstance() {
    static StorageService instance;
    return instance;
}

StorageService::StorageService() {
    m_file_mutex = xSemaphoreCreateMutex();
    if (!m_file_mutex) {
        ESP_LOGE(TAG, "Failed to create file mutex!");
    }
}

StorageService::~StorageService() {
    if (m_file_mutex) {
        vSemaphoreDelete(m_file_mutex);
        m_file_mutex = nullptr;
    }
}

bool StorageService::isMounted() const {
    return Board::getInstance().getStorage().isMounted();
}

bool StorageService::writeFile(const char* path, const char* content) {
    if (!isMounted()) {
        ESP_LOGE(TAG, "Cannot write to %s: SD card not mounted", path);
        return false;
    }

    FreeRTOSLock lock(m_file_mutex);
    FILE* f = fopen(path, "w");
    if (f == nullptr) {
        ESP_LOGE(TAG, "Failed to open file %s for writing", path);
        return false;
    }

    int bytes_written = fprintf(f, "%s", content);
    fclose(f);

    if (bytes_written < 0) {
        ESP_LOGE(TAG, "Failed to write content to %s", path);
        return false;
    }

    ESP_LOGI(TAG, "Successfully wrote %d bytes to %s", bytes_written, path);
    return true;
}

bool StorageService::appendFile(const char* path, const char* content) {
    if (!isMounted()) {
        ESP_LOGE(TAG, "Cannot append to %s: SD card not mounted", path);
        return false;
    }

    FreeRTOSLock lock(m_file_mutex);
    FILE* f = fopen(path, "a");
    if (f == nullptr) {
        ESP_LOGE(TAG, "Failed to open file %s for appending", path);
        return false;
    }

    int bytes_written = fprintf(f, "%s", content);
    fclose(f);

    if (bytes_written < 0) {
        ESP_LOGE(TAG, "Failed to append content to %s", path);
        return false;
    }

    ESP_LOGI(TAG, "Successfully appended %d bytes to %s", bytes_written, path);
    return true;
}

std::string StorageService::readFile(const char* path) {
    if (!isMounted()) {
        ESP_LOGE(TAG, "Cannot read from %s: SD card not mounted", path);
        return "";
    }

    FreeRTOSLock lock(m_file_mutex);
    FILE* f = fopen(path, "r");
    if (f == nullptr) {
        ESP_LOGW(TAG, "Failed to open file %s for reading", path);
        return "";
    }

    // Seek to end to get file size
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return "";
    }

    std::string content;
    content.resize(size);
    size_t read_bytes = fread(&content[0], 1, size, f);
    fclose(f);

    if (read_bytes < size) {
        content.resize(read_bytes);
    }

    return content;
}

bool StorageService::deleteFile(const char* path) {
    if (!isMounted()) {
        ESP_LOGE(TAG, "Cannot delete %s: SD card not mounted", path);
        return false;
    }

    FreeRTOSLock lock(m_file_mutex);
    if (remove(path) != 0) {
        ESP_LOGE(TAG, "Failed to remove file: %s", path);
        return false;
    }

    ESP_LOGI(TAG, "Deleted file: %s", path);
    return true;
}

bool StorageService::fileExists(const char* path) {
    if (!isMounted()) {
        return false;
    }

    FreeRTOSLock lock(m_file_mutex);
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

void StorageService::listFiles(const char* dir_path, const char* extension, FileFoundCallback cb, void* ctx) {
    if (!isMounted() || cb == nullptr) {
        return;
    }

    FreeRTOSLock lock(m_file_mutex);
    DIR* dir = opendir(dir_path);
    if (dir == nullptr) {
        ESP_LOGE(TAG, "Failed to open directory: %s", dir_path);
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        // Skip "." and ".."
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // Filter by extension if specified
        if (extension != nullptr) {
            const char* ext = strrchr(entry->d_name, '.');
            if (ext == nullptr || strcasecmp(ext, extension) != 0) {
                continue;
            }
        }

        // Trigger callback for the matching file
        cb(entry->d_name, ctx);
    }

    closedir(dir);
}

FILE* StorageService::openStream(const char* path, const char* mode) {
    if (!isMounted()) return nullptr;
    return fopen(path, mode);
}

size_t StorageService::writeStream(FILE* stream, const void* buffer, size_t size) {
    if (!stream || !buffer || size == 0) return 0;
    return fwrite(buffer, 1, size, stream);
}

size_t StorageService::readStream(FILE* stream, void* buffer, size_t size) {
    if (!stream || !buffer || size == 0) return 0;
    return fread(buffer, 1, size, stream);
}

bool StorageService::isStreamEOF(FILE* stream) {
    if (!stream) return true;
    return feof(stream) != 0;
}

void StorageService::closeStream(FILE* stream) {
    if (!stream) return;
    fflush(stream);
    fclose(stream);
}

void StorageService::lock() {
    if (m_file_mutex) {
        xSemaphoreTake(m_file_mutex, portMAX_DELAY);
    }
}

void StorageService::unlock() {
    if (m_file_mutex) {
        xSemaphoreGive(m_file_mutex);
    }
}

bool StorageService::getStorageInfo(const char* base_path, uint64_t& total_bytes, uint64_t& free_bytes) {
    if (!isMounted()) {
        total_bytes = 0;
        free_bytes = 0;
        return false;
    }
    FreeRTOSLock lock(m_file_mutex);
    esp_err_t err = esp_vfs_fat_info(base_path, &total_bytes, &free_bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_vfs_fat_info failed for %s: %s", base_path, esp_err_to_name(err));
        return false;
    }
    return true;
}

std::vector<StorageService::FileEntryInfo> StorageService::listDirectoryDetailed(const char* dir_path) {
    std::vector<FileEntryInfo> entries;
    if (!isMounted()) {
        return entries;
    }

    FreeRTOSLock lock(m_file_mutex);
    DIR* dir = opendir(dir_path);
    if (dir == nullptr) {
        ESP_LOGW(TAG, "Failed to open directory: %s", dir_path);
        return entries;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        FileEntryInfo info;
        info.name = entry->d_name;

        std::string full_path = dir_path;
        if (full_path.empty() || full_path.back() != '/') {
            full_path += '/';
        }
        full_path += entry->d_name;

        struct stat st;
        if (stat(full_path.c_str(), &st) == 0) {
            info.size = st.st_size;
            info.is_dir = S_ISDIR(st.st_mode);
            info.mtime = st.st_mtime;
        } else {
            info.size = 0;
            info.is_dir = false;
            info.mtime = 0;
        }

        entries.push_back(info);
    }
    closedir(dir);

    std::sort(entries.begin(), entries.end(), [](const FileEntryInfo& a, const FileEntryInfo& b) {
        if (a.is_dir != b.is_dir) {
            return a.is_dir > b.is_dir;
        }
        return a.name < b.name;
    });

    return entries;
}

bool StorageService::createDirectory(const char* dir_path) {
    if (!isMounted()) return false;
    FreeRTOSLock lock(m_file_mutex);
    int res = mkdir(dir_path, 0755);
    if (res != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "Failed to create directory %s (errno %d)", dir_path, errno);
        return false;
    }
    return true;
}

bool StorageService::renamePath(const char* old_path, const char* new_path) {
    if (!isMounted()) return false;
    FreeRTOSLock lock(m_file_mutex);
    if (rename(old_path, new_path) != 0) {
        ESP_LOGE(TAG, "Failed to rename %s to %s (errno %d)", old_path, new_path, errno);
        return false;
    }
    return true;
}

bool StorageService::deletePath(const char* path) {
    if (!isMounted()) return false;
    FreeRTOSLock lock(m_file_mutex);
    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGW(TAG, "deletePath target not found: %s", path);
        return false;
    }
    int res = 0;
    if (S_ISDIR(st.st_mode)) {
        res = rmdir(path);
    } else {
        res = remove(path);
    }
    if (res != 0) {
        ESP_LOGE(TAG, "Failed to delete %s (errno %d)", path, errno);
        return false;
    }
    return true;
}

bool StorageService::sanitizePath(const char* in_path, std::string& out_path, const char* base_mount) {
    if (base_mount == nullptr || base_mount[0] == '\0') {
        base_mount = "/sdcard";
    }
    if (in_path == nullptr || in_path[0] == '\0') {
        out_path = base_mount;
        return true;
    }

    std::string raw = in_path;
    // Disallow directory traversal
    if (raw.find("..") != std::string::npos) {
        return false;
    }

    for (char& c : raw) {
        if (c == '\\') c = '/';
    }

    std::string normalized;
    normalized.reserve(raw.size());
    bool last_was_slash = false;
    for (char c : raw) {
        if (c == '/') {
            if (!last_was_slash) {
                normalized.push_back(c);
                last_was_slash = true;
            }
        } else {
            normalized.push_back(c);
            last_was_slash = false;
        }
    }

    size_t base_len = strlen(base_mount);
    if (normalized.compare(0, base_len, base_mount) != 0) {
        if (normalized.empty() || normalized[0] != '/') {
            out_path = std::string(base_mount) + "/" + normalized;
        } else {
            out_path = std::string(base_mount) + normalized;
        }
    } else {
        out_path = normalized;
    }

    while (out_path.size() > base_len && out_path.back() == '/') {
        out_path.pop_back();
    }

    return true;
}

} // namespace Services
