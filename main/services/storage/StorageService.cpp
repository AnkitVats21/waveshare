#include "StorageService.h"
#include "sd_storage/Fs.h"
#include "sd_storage/PathPolicy.h"
#include "sd_storage/SdCard.h"
#include "esp_log.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* TAG = "StorageService";

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
    return sd_storage::SdCard::instance().isMounted();
}

// Whole-file and directory operations are handled by sd_storage, which checks
// for conflicting opens itself instead of taking the global mutex.
bool StorageService::writeFile(const char* path, const char* content) {
    return sd_storage::Fs::writeAtomic(path, content);
}

bool StorageService::appendFile(const char* path, const char* content) {
    return content && sd_storage::Fs::append(path, content, strlen(content));
}

std::string StorageService::readFile(const char* path) {
    return sd_storage::Fs::readText(path);
}

bool StorageService::deleteFile(const char* path) {
    return sd_storage::Fs::remove(path);
}

bool StorageService::fileExists(const char* path) {
    return sd_storage::Fs::isFile(path);
}

void StorageService::listFiles(const char* dir_path, const char* extension, FileFoundCallback cb, void* ctx) {
    if (cb == nullptr) return;
    struct Adapter { FileFoundCallback cb; void* ctx; } a{cb, ctx};
    sd_storage::Fs::list(dir_path, extension, false, [](const sd_storage::DirEntry& e, void* p) {
        auto* ad = static_cast<Adapter*>(p);
        ad->cb(e.name, ad->ctx);
        return true;
    }, &a);
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
    (void)base_path;
    return sd_storage::Fs::info(total_bytes, free_bytes);
}

std::vector<StorageService::FileEntryInfo> StorageService::listDirectoryDetailed(const char* dir_path) {
    std::vector<FileEntryInfo> entries;
    sd_storage::Fs::list(dir_path, nullptr, true, [](const sd_storage::DirEntry& e, void* p) {
        static_cast<std::vector<FileEntryInfo>*>(p)->push_back({e.name, e.size, e.is_dir, e.mtime});
        return true;
    }, &entries);

    std::sort(entries.begin(), entries.end(), [](const FileEntryInfo& a, const FileEntryInfo& b) {
        if (a.is_dir != b.is_dir) {
            return a.is_dir > b.is_dir;
        }
        return a.name < b.name;
    });

    return entries;
}

bool StorageService::createDirectory(const char* dir_path) {
    return sd_storage::Fs::mkdirs(dir_path);
}

bool StorageService::renamePath(const char* old_path, const char* new_path) {
    return sd_storage::Fs::rename(old_path, new_path);
}

bool StorageService::deletePath(const char* path) {
    return sd_storage::Fs::removePath(path);
}

bool StorageService::sanitizePath(const char* in_path, std::string& out_path, const char* base_mount) {
    return sd_storage::PathPolicy::sanitize(in_path, out_path, base_mount);
}

} // namespace Services
