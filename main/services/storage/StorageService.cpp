#include "StorageService.h"
#include "hal/Board.h"
#include "esp_log.h"
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <cstdio>
#include <cstring>

static const char* TAG = "StorageService";

namespace Services {

StorageService& StorageService::getInstance() {
    static StorageService instance;
    return instance;
}

bool StorageService::isMounted() const {
    return Board::getInstance().getStorage().isMounted();
}

bool StorageService::writeFile(const char* path, const char* content) {
    if (!isMounted()) {
        ESP_LOGE(TAG, "Cannot write to %s: SD card not mounted", path);
        return false;
    }

    std::lock_guard<std::mutex> lock(m_file_mutex);
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

std::string StorageService::readFile(const char* path) {
    if (!isMounted()) {
        ESP_LOGE(TAG, "Cannot read from %s: SD card not mounted", path);
        return "";
    }

    std::lock_guard<std::mutex> lock(m_file_mutex);
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

    std::lock_guard<std::mutex> lock(m_file_mutex);
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

    std::lock_guard<std::mutex> lock(m_file_mutex);
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

void StorageService::listFiles(const char* dir_path, const char* extension, FileFoundCallback cb, void* ctx) {
    if (!isMounted() || cb == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_file_mutex);
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

} // namespace Services
