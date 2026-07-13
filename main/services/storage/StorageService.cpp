#include "StorageService.h"
#include "hal/Board.h"
#include "esp_log.h"
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <cstdio>
#include <cstring>
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

} // namespace Services
