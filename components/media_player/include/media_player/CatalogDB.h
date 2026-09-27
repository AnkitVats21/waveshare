#pragma once

#include <cstdint>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

#include "MusicDb.generated.h"

// One library entry: the video id and its music.ndb document.
struct LibraryTrack {
    std::string id;
    ndb::music::TrackDoc doc;
};

// The music library: /sdcard/db/music.ndb (schema/db/music.star), one entry
// per song played or found on the card. An entry whose file is gone stays
// with file_size 0 ("not saved"), keeping its play history.
//
// Thread safe. Writes are batched; flushIfDue() is called from media_aux.
class CatalogDB {
public:
    static CatalogDB& getInstance();

    // Opens music.ndb; the first time, imports /sdcard/music/catalog.db.
    bool begin();
    // For GET /api/db/music.
    nexus_db::Database& database() { return _db.db(); }

    bool get(const char* videoId, ndb::music::TrackDoc& out);
    bool exists(const char* videoId);
    // Title, artist or id containing `query`, ignoring case.
    std::vector<LibraryTrack> search(const char* query);

    // A song started playing from the network: adds it, or refreshes its
    // title, artist and length (empty or 0 leaves the stored value).
    void noteTrack(const char* videoId, const std::string& title, const std::string& artist, uint32_t durationMs);
    void recordPlay(const char* videoId);
    void setDurationIfUnknown(const char* videoId, uint32_t durationMs);
    void setThumbnail(const char* videoId, bool present);
    // A download was committed to the card (0: its file was deleted).
    void setSaved(const char* videoId, uint32_t fileSize);
    // Deletes the saved file and thumbnail; the entry stays as not saved.
    // False if the id isn't in the library or a file couldn't be deleted
    // (e.g. it is open).
    bool removeFiles(const char* videoId);

    // Checks the library against the card: adds songs that aren't listed,
    // corrects file sizes and thumbnail flags, marks entries whose file is
    // gone as not saved, and deletes leftover download .tmp files (not the
    // one in progress). Runs at boot on media_aux, and on a library scan.
    // Returns the number of audio files on the card.
    size_t scanAndSync();

    void flushIfDue() { _db.db().flushIfDue(); }

    static constexpr const char* MUSIC_DIR  = "/sdcard/music";
    static constexpr const char* THUMBS_DIR = "/sdcard/music/thumbs";

private:
    CatalogDB() = default;
    CatalogDB(const CatalogDB&) = delete;
    CatalogDB& operator=(const CatalogDB&) = delete;

    void importCatalogDb();
    static bool thumbnailExists(const char* videoId);

    ndb::music::MusicDb _db;
    std::mutex _mutex;  // read-modify-write sequences (play count, scan)
    bool _initialized = false;
};
