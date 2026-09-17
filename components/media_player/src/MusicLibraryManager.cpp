#include "media_player/MusicLibraryManager.h"
#include <ArduinoJson.h>
#include <esp_log.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <algorithm>
#include <fstream>
#include <sstream>

static const char* TAG = "MusicLib";

MusicLibraryManager& MusicLibraryManager::getInstance() {
    static MusicLibraryManager instance;
    return instance;
}

MusicLibraryManager::MusicLibraryManager() {}

bool MusicLibraryManager::begin() {
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    // Ensure music and thumbs directories exist
    mkdir(MUSIC_DIR, 0777);
    mkdir(THUMBS_DIR, 0777);

    if (!loadFromFile()) {
        ESP_LOGI(TAG, "library.json not found or empty. Performing initial scan of %s...", MUSIC_DIR);
        scanAndSync();
    } else {
        ESP_LOGI(TAG, "Loaded %u tracks from %s", static_cast<unsigned>(_tracks.size()), LIBRARY_PATH);
    }
    _loaded = true;
    return true;
}

bool MusicLibraryManager::loadFromFile() {
    FILE* f = fopen(LIBRARY_PATH, "r");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 512 * 1024) {
        fclose(f);
        return false;
    }

    std::string jsonStr;
    jsonStr.resize(size);
    size_t readBytes = fread(&jsonStr[0], 1, size, f);
    fclose(f);

    if (readBytes != static_cast<size_t>(size)) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, jsonStr);
    if (err) {
        ESP_LOGW(TAG, "Failed to parse %s: %s", LIBRARY_PATH, err.c_str());
        return false;
    }

    _tracks.clear();
    JsonArray arr = doc["tracks"].as<JsonArray>();
    for (JsonObject obj : arr) {
        LibraryTrack t;
        t.id              = obj["id"] | "";
        t.title           = obj["title"] | "";
        t.artist          = obj["artist"] | "";
        t.durationSeconds = obj["duration"] | 0;
        t.sizeBytes       = obj["size_bytes"] | 0;
        t.format          = obj["format"] | "opus";
        t.filePath        = obj["file"] | "";
        t.hasThumbnail    = obj["has_thumb"] | false;
        t.cachedAt        = obj["cached_at"] | 0;

        if (!t.id.empty() && !t.filePath.empty()) {
            _tracks.push_back(t);
        }
    }

    return true;
}

bool MusicLibraryManager::saveToFile() {
    JsonDocument doc;
    doc["version"] = 1;
    doc["total_tracks"] = _tracks.size();

    JsonArray arr = doc["tracks"].to<JsonArray>();
    for (const auto& t : _tracks) {
        JsonObject obj = arr.add<JsonObject>();
        obj["id"]         = t.id;
        obj["title"]      = t.title;
        obj["artist"]     = t.artist;
        obj["duration"]   = t.durationSeconds;
        obj["size_bytes"] = t.sizeBytes;
        obj["format"]     = t.format;
        obj["file"]       = t.filePath;
        obj["has_thumb"]  = t.hasThumbnail;
        obj["cached_at"]  = t.cachedAt;
    }

    std::string out;
    serializeJson(doc, out);

    FILE* f = fopen(LIBRARY_PATH, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", LIBRARY_PATH);
        return false;
    }

    size_t written = fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return (written == out.size());
}

bool MusicLibraryManager::addOrUpdateTrack(const LibraryTrack& track) {
    if (track.id.empty()) return false;
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    // Check if thumbnail exists
    LibraryTrack updated = track;
    std::string thumbPath = std::string(THUMBS_DIR) + "/" + updated.id + ".jpg";
    struct stat st;
    if (stat(thumbPath.c_str(), &st) == 0 && st.st_size > 0) {
        updated.hasThumbnail = true;
    }

    // Check file size if available
    if (!updated.filePath.empty() && stat(updated.filePath.c_str(), &st) == 0) {
        updated.sizeBytes = st.st_size;
    }

    auto it = std::find_if(_tracks.begin(), _tracks.end(),
                           [&updated](const LibraryTrack& t) { return t.id == updated.id; });
    if (it != _tracks.end()) {
        *it = updated;
    } else {
        _tracks.push_back(updated);
    }

    saveToFile();
    ESP_LOGI(TAG, "Indexed track in local library: '%s' by '%s' (id=%s)",
             updated.title.c_str(), updated.artist.c_str(), updated.id.c_str());
    return true;
}

bool MusicLibraryManager::removeTrack(const std::string& id) {
    if (id.empty()) return false;
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    auto it = std::find_if(_tracks.begin(), _tracks.end(),
                           [&id](const LibraryTrack& t) { return t.id == id; });
    if (it == _tracks.end()) return false;

    // Delete audio file
    if (!it->filePath.empty()) {
        unlink(it->filePath.c_str());
    }
    // Delete thumbnail
    std::string thumbPath = std::string(THUMBS_DIR) + "/" + id + ".jpg";
    unlink(thumbPath.c_str());

    _tracks.erase(it);
    saveToFile();
    ESP_LOGI(TAG, "Removed track id=%s from local library", id.c_str());
    return true;
}

bool MusicLibraryManager::getTrack(const std::string& id, LibraryTrack& outTrack) {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    auto it = std::find_if(_tracks.begin(), _tracks.end(),
                           [&id](const LibraryTrack& t) { return t.id == id; });
    if (it != _tracks.end()) {
        outTrack = *it;
        return true;
    }
    return false;
}

std::vector<LibraryTrack> MusicLibraryManager::getAllTracks() {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    return _tracks;
}

std::vector<LibraryTrack> MusicLibraryManager::search(const std::string& query) {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    if (query.empty()) return _tracks;

    std::string qLower = query;
    std::transform(qLower.begin(), qLower.end(), qLower.begin(), ::tolower);

    std::vector<LibraryTrack> matches;
    for (const auto& t : _tracks) {
        std::string tTitle = t.title;
        std::string tArtist = t.artist;
        std::transform(tTitle.begin(), tTitle.end(), tTitle.begin(), ::tolower);
        std::transform(tArtist.begin(), tArtist.end(), tArtist.begin(), ::tolower);

        if (tTitle.find(qLower) != std::string::npos ||
            tArtist.find(qLower) != std::string::npos ||
            t.id.find(query) != std::string::npos) {
            matches.push_back(t);
        }
    }
    return matches;
}

size_t MusicLibraryManager::scanAndSync() {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    mkdir(MUSIC_DIR, 0777);
    mkdir(THUMBS_DIR, 0777);

    DIR* dir = opendir(MUSIC_DIR);
    if (!dir) return _tracks.size();

    std::vector<std::string> diskFiles;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_DIR) continue;
        std::string fname = entry->d_name;
        if (fname == "library.json") continue;

        size_t dot = fname.rfind('.');
        if (dot == std::string::npos) continue;

        std::string ext = fname.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == ".opus" || ext == ".ogg" || ext == ".wav" || ext == ".mp3") {
            diskFiles.push_back(fname);
        }
    }
    closedir(dir);

    // 1. Add any newly found files to _tracks
    for (const auto& fname : diskFiles) {
        size_t dot = fname.rfind('.');
        std::string baseId = fname.substr(0, dot);
        std::string ext = fname.substr(dot + 1);
        std::string fullPath = std::string(MUSIC_DIR) + "/" + fname;

        auto it = std::find_if(_tracks.begin(), _tracks.end(),
                               [&baseId](const LibraryTrack& t) { return t.id == baseId; });
        if (it == _tracks.end()) {
            LibraryTrack newTrack;
            newTrack.id = baseId;
            newTrack.title = cleanTitleFromFilename(baseId);
            newTrack.artist = "Local";
            newTrack.format = ext;
            newTrack.filePath = fullPath;
            struct stat st;
            if (stat(fullPath.c_str(), &st) == 0) {
                newTrack.sizeBytes = st.st_size;
            }
            std::string thumb = std::string(THUMBS_DIR) + "/" + baseId + ".jpg";
            newTrack.hasThumbnail = (stat(thumb.c_str(), &st) == 0);
            _tracks.push_back(newTrack);
        } else {
            // Update path and size if changed
            it->filePath = fullPath;
            struct stat st;
            if (stat(fullPath.c_str(), &st) == 0) {
                it->sizeBytes = st.st_size;
            }
            std::string thumb = std::string(THUMBS_DIR) + "/" + baseId + ".jpg";
            it->hasThumbnail = (stat(thumb.c_str(), &st) == 0);
        }
    }

    // 2. Remove any tracks whose files were deleted on disk
    _tracks.erase(std::remove_if(_tracks.begin(), _tracks.end(), [](const LibraryTrack& t) {
        struct stat st;
        return (stat(t.filePath.c_str(), &st) != 0);
    }), _tracks.end());

    saveToFile();
    ESP_LOGI(TAG, "Library sync complete: %u tracks on SD card", static_cast<unsigned>(_tracks.size()));
    return _tracks.size();
}

std::string MusicLibraryManager::serializeLibraryJson(const std::string& filter) {
    auto trackList = filter.empty() ? getAllTracks() : search(filter);

    JsonDocument doc;
    doc["count"] = trackList.size();
    JsonArray arr = doc["tracks"].to<JsonArray>();

    for (const auto& t : trackList) {
        JsonObject obj = arr.add<JsonObject>();
        obj["id"]         = t.id;
        obj["title"]      = t.title;
        obj["artist"]     = t.artist;
        obj["duration"]   = t.durationSeconds;
        obj["size_bytes"] = t.sizeBytes;
        obj["format"]     = t.format;
        obj["file"]       = t.filePath;
        obj["has_thumb"]  = t.hasThumbnail;
    }

    std::string out;
    serializeJson(doc, out);
    return out;
}

std::string MusicLibraryManager::cleanTitleFromFilename(const std::string& filename) {
    std::string clean = filename;
    for (char& c : clean) {
        if (c == '_' || c == '-') c = ' ';
    }
    return clean;
}
