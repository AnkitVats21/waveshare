#pragma once

#include "InvidiousClient.h"
#include <string>
#include <vector>
#include <mutex>

struct LibraryTrack {
    std::string id;              ///< videoId or file base name (e.g. "dQw4w9WgXcQ")
    std::string title;           ///< Display title
    std::string artist;          ///< Artist / channel / uploader
    int durationSeconds = 0;     ///< Track length in seconds
    size_t sizeBytes = 0;        ///< Size on SD card
    std::string format = "opus"; ///< opus, ogg, wav, mp3
    std::string filePath;        ///< /sdcard/music/dQw4w9WgXcQ.opus
    bool hasThumbnail = false;   ///< Whether /sdcard/music/thumbs/<id>.jpg exists
    uint32_t cachedAt = 0;       ///< Epoch timestamp or relative time added
};

class MusicLibraryManager {
public:
    static MusicLibraryManager& getInstance();

    bool begin();

    // Track CRUD
    bool addOrUpdateTrack(const LibraryTrack& track);
    bool removeTrack(const std::string& id);
    bool getTrack(const std::string& id, LibraryTrack& outTrack);
    std::vector<LibraryTrack> getAllTracks();
    std::vector<LibraryTrack> search(const std::string& query);

    // Sync filesystem with library.json (indexes new .opus/.ogg/.wav/.mp3 files and purges missing ones)
    size_t scanAndSync();

    // Serializes the library list to JSON
    std::string serializeLibraryJson(const std::string& filter = "");

    static constexpr const char* MUSIC_DIR    = "/sdcard/music";
    static constexpr const char* THUMBS_DIR   = "/sdcard/music/thumbs";
    static constexpr const char* LIBRARY_PATH = "/sdcard/music/library.json";

private:
    MusicLibraryManager();
    ~MusicLibraryManager() = default;
    MusicLibraryManager(const MusicLibraryManager&) = delete;
    MusicLibraryManager& operator=(const MusicLibraryManager&) = delete;

    mutable std::recursive_mutex _mutex;
    std::vector<LibraryTrack> _tracks;
    bool _loaded = false;

    bool loadFromFile();
    bool saveToFile();
    static std::string cleanTitleFromFilename(const std::string& filename);
};
