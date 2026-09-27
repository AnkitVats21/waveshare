#include "media_player/CatalogDB.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "sd_storage/File.h"
#include "sd_storage/Fs.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <memory>

static const char* TAG = "CatalogDB";

using ndb::music::TrackDoc;
using sd_storage::File;
using sd_storage::Mode;
namespace Fs = sd_storage::Fs;

namespace {

// The files music.ndb replaced; imported once, then renamed .bak.
constexpr const char* OLD_CATALOG = "/sdcard/music/catalog.db";
constexpr const char* OLD_WAL = "/sdcard/music/catalog.wal";
constexpr uint32_t OLD_MAGIC = 0xCAFEBEEF;
constexpr uint8_t OLD_FLAG_VALID = 1 << 0;
constexpr uint8_t OLD_FLAG_HAS_THUMBNAIL = 1 << 4;

#pragma pack(push, 1)
// catalog.db's 1 KB record (version 2), the fields the import reads.
struct OldRecord {
    uint32_t magic;
    uint16_t version;
    uint8_t flags;
    uint8_t _pad0[9];
    char videoId[32];
    char title[64];
    char artist[32];
    char album[32];
    uint32_t durationMs;
    uint32_t fileSizeBytes;
    uint32_t avgBitrateBps;
    uint32_t cachedAt;
    uint32_t lastPlayedAt;
    uint32_t playCount;
    uint32_t streamDurationMs;
    uint8_t _rest[1024 - 204];
};
struct OldWalEntry {
    uint32_t crc32;  // of everything after this field
    uint32_t timestamp;
    uint8_t opType;  // 1 upsert, 2 delete, 3 seek table (a whole record too)
    uint8_t _pad[3];
    char videoId[32];
    OldRecord record;
};
#pragma pack(pop)
static_assert(sizeof(OldRecord) == 1024, "catalog.db record layout");

// A fixed-size C string field; titles were cut to fit it, which can split a
// multi-byte character, so an incomplete one at the end is dropped.
std::string fixedString(const char* s, size_t n) {
    size_t len = strnlen(s, n);
    size_t start = len;
    while (start > 0 && (static_cast<uint8_t>(s[start - 1]) & 0xC0) == 0x80) --start;
    if (start > 0 && static_cast<uint8_t>(s[start - 1]) >= 0xC0) {
        const uint8_t lead = static_cast<uint8_t>(s[start - 1]);
        const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
        if (len - (start - 1) < need) len = start - 1;
    }
    return std::string(s, len);
}

TrackDoc fromOld(const OldRecord& r) {
    TrackDoc d;
    d.title = fixedString(r.title, sizeof(r.title));
    d.artist = fixedString(r.artist, sizeof(r.artist));
    d.album = fixedString(r.album, sizeof(r.album));
    d.duration_ms = r.durationMs ? r.durationMs : r.streamDurationMs;
    d.file_size = r.fileSizeBytes;
    d.added_at = r.cachedAt;
    d.last_played_at = r.lastPlayedAt;
    d.play_count = r.playCount;
    d.thumbnail = (r.flags & OLD_FLAG_HAS_THUMBNAIL) != 0;
    return d;
}

bool containsIgnoreCase(const std::string& str, const std::string& sub) {
    if (sub.empty()) return true;
    return std::search(str.begin(), str.end(), sub.begin(), sub.end(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           }) != str.end();
}

bool isAudioFile(const std::string& name, std::string& id) {
    for (const char* ext : {".webm", ".opus", ".ogg"}) {
        const size_t n = strlen(ext);
        if (name.size() > n && name.compare(name.size() - n, n, ext) == 0) {
            id = name.substr(0, name.size() - n);
            return true;
        }
    }
    return false;
}

std::string titleFromFileName(std::string title) {
    for (char& c : title) {
        if (c == '_' || c == '-') c = ' ';
    }
    return title;
}

uint32_t now() { return static_cast<uint32_t>(time(nullptr)); }

} // namespace

CatalogDB& CatalogDB::getInstance() {
    static CatalogDB instance;
    return instance;
}

bool CatalogDB::begin() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_initialized) return true;

    Fs::mkdirs(THUMBS_DIR);  // and MUSIC_DIR above it
    if (!_db.open()) {
        ESP_LOGE(TAG, "Cannot open music.ndb");
        return false;
    }
    if (_db.tracks().count() == 0 && Fs::isFile(OLD_CATALOG)) importCatalogDb();

    _initialized = true;
    ESP_LOGI(TAG, "Music library: %u tracks", (unsigned)_db.tracks().count());
    return true;
}

void CatalogDB::importCatalogDb() {
    auto tracks = _db.tracks();
    size_t imported = 0, walApplied = 0;
    bool readOk = false;
    {
        File f = File::open(OLD_CATALOG, Mode::Read);
        auto rec = std::make_unique<OldRecord>();
        readOk = static_cast<bool>(f);
        while (f && f.readExact(rec.get(), sizeof(OldRecord))) {
            if (rec->magic != OLD_MAGIC || !(rec->flags & OLD_FLAG_VALID)) continue;
            const std::string id = fixedString(rec->videoId, sizeof(rec->videoId));
            if (!id.empty() && tracks.put(id, fromOld(*rec))) ++imported;
        }
    }
    // Normally absent: each catalog write removed it after landing.
    {
        File f = File::open(OLD_WAL, Mode::Read);
        auto entry = std::make_unique<OldWalEntry>();
        while (f && f.readExact(entry.get(), sizeof(OldWalEntry))) {
            const auto* p = reinterpret_cast<const uint8_t*>(entry.get()) + sizeof(uint32_t);
            if (entry->crc32 != esp_rom_crc32_le(0, p, sizeof(OldWalEntry) - sizeof(uint32_t))) continue;
            const std::string id = fixedString(entry->videoId, sizeof(entry->videoId));
            if (id.empty()) continue;
            if (entry->opType == 1 || entry->opType == 3) {
                tracks.put(id, fromOld(entry->record));
                ++walApplied;
            } else if (entry->opType == 2) {
                tracks.remove(id);
                ++walApplied;
            }
        }
    }
    _db.db().flush();
    ESP_LOGI(TAG, "Imported %u tracks from catalog.db (%u journal entries)", (unsigned)imported,
             (unsigned)walApplied);

    // Renamed only once the import is on the card; a failed read leaves the
    // old file in place for the next boot to try again.
    if (!readOk) return;
    const std::string bak = std::string(OLD_CATALOG) + ".bak";
    Fs::remove(bak.c_str());
    if (!Fs::rename(OLD_CATALOG, bak.c_str())) ESP_LOGW(TAG, "Could not rename catalog.db to .bak");
    if (Fs::isFile(OLD_WAL)) {
        const std::string walBak = std::string(OLD_WAL) + ".bak";
        Fs::remove(walBak.c_str());
        Fs::rename(OLD_WAL, walBak.c_str());
    }
}

bool CatalogDB::get(const char* videoId, TrackDoc& out) {
    if (!videoId || videoId[0] == '\0') return false;
    return _db.tracks().get(videoId, out);
}

bool CatalogDB::exists(const char* videoId) {
    if (!videoId || videoId[0] == '\0') return false;
    return _db.tracks().contains(videoId);
}

std::vector<LibraryTrack> CatalogDB::search(const char* query) {
    std::vector<LibraryTrack> result;
    if (!query) return result;
    const std::string q = query;
    _db.tracks().forEach([&](std::string_view key, const TrackDoc& doc) {
        std::string id(key);
        if (containsIgnoreCase(doc.title, q) || containsIgnoreCase(doc.artist, q) || containsIgnoreCase(id, q)) {
            result.push_back({std::move(id), doc});
        }
        return true;
    });
    return result;
}

void CatalogDB::noteTrack(const char* videoId, const std::string& title, const std::string& artist,
                          uint32_t durationMs) {
    if (!videoId || videoId[0] == '\0') return;
    std::lock_guard<std::mutex> lock(_mutex);
    auto tracks = _db.tracks();
    TrackDoc doc;
    if (!tracks.get(videoId, doc)) {
        doc.title = title.empty() || title == videoId ? videoId : title;
        doc.artist = artist;
        doc.duration_ms = durationMs;
        doc.added_at = now();
        doc.thumbnail = thumbnailExists(videoId);
        tracks.put(videoId, doc);
        return;
    }
    uint64_t fields = 0;
    if (!title.empty() && title != videoId && title != doc.title) {
        doc.title = title;
        fields |= TrackDoc::F_TITLE;
    }
    if (!artist.empty() && artist != doc.artist) {
        doc.artist = artist;
        fields |= TrackDoc::F_ARTIST;
    }
    if (durationMs > 0 && durationMs != doc.duration_ms) {
        doc.duration_ms = durationMs;
        fields |= TrackDoc::F_DURATION_MS;
    }
    if (fields) tracks.merge(videoId, doc, fields);
}

void CatalogDB::recordPlay(const char* videoId) {
    if (!videoId || videoId[0] == '\0') return;
    std::lock_guard<std::mutex> lock(_mutex);
    TrackDoc doc;
    if (!_db.tracks().get(videoId, doc)) return;
    doc.play_count++;
    doc.last_played_at = now();
    _db.tracks().merge(videoId, doc, TrackDoc::F_PLAY_COUNT | TrackDoc::F_LAST_PLAYED_AT);
}

void CatalogDB::setDurationIfUnknown(const char* videoId, uint32_t durationMs) {
    if (!videoId || videoId[0] == '\0' || durationMs == 0) return;
    std::lock_guard<std::mutex> lock(_mutex);
    TrackDoc doc;
    if (!_db.tracks().get(videoId, doc) || doc.duration_ms != 0) return;
    doc.duration_ms = durationMs;
    _db.tracks().merge(videoId, doc, TrackDoc::F_DURATION_MS);
}

void CatalogDB::setThumbnail(const char* videoId, bool present) {
    if (!videoId || videoId[0] == '\0') return;
    std::lock_guard<std::mutex> lock(_mutex);
    TrackDoc doc;
    if (!_db.tracks().get(videoId, doc) || doc.thumbnail == present) return;
    doc.thumbnail = present;
    _db.tracks().merge(videoId, doc, TrackDoc::F_THUMBNAIL);
}

void CatalogDB::setSaved(const char* videoId, uint32_t fileSize) {
    if (!videoId || videoId[0] == '\0') return;
    std::lock_guard<std::mutex> lock(_mutex);
    TrackDoc doc;
    if (!_db.tracks().get(videoId, doc)) {
        if (fileSize == 0) return;
        doc.title = videoId;
        doc.added_at = now();
        doc.file_size = fileSize;
        doc.thumbnail = thumbnailExists(videoId);
        _db.tracks().put(videoId, doc);
        return;
    }
    if (doc.file_size == fileSize) return;
    doc.file_size = fileSize;
    _db.tracks().merge(videoId, doc, TrackDoc::F_FILE_SIZE);
}

bool CatalogDB::removeFiles(const char* videoId) {
    if (!videoId || videoId[0] == '\0') return false;
    std::lock_guard<std::mutex> lock(_mutex);
    TrackDoc doc;
    if (!_db.tracks().get(videoId, doc)) return false;

    bool ok = true;
    char path[128];
    for (const char* ext : {".webm", ".opus", ".ogg"}) {
        snprintf(path, sizeof(path), "%s/%s%s", MUSIC_DIR, videoId, ext);
        if (Fs::isFile(path) && !Fs::remove(path)) {
            ESP_LOGW(TAG, "Could not delete %s", path);
            ok = false;
        }
    }
    snprintf(path, sizeof(path), "%s/%s.jpg", THUMBS_DIR, videoId);
    if (Fs::isFile(path)) Fs::remove(path);

    if (ok) doc.file_size = 0;
    doc.thumbnail = thumbnailExists(videoId);
    _db.tracks().merge(videoId, doc, TrackDoc::F_FILE_SIZE | TrackDoc::F_THUMBNAIL);
    return ok;
}

size_t CatalogDB::scanAndSync() {
    std::lock_guard<std::mutex> lock(_mutex);
    struct ScanCtx {
        CatalogDB* self;
        size_t seen;
    } ctx{this, 0};

    Fs::list(MUSIC_DIR, nullptr, true, [](const sd_storage::DirEntry& ent, void* p) {
        auto* c = static_cast<ScanCtx*>(p);
        std::string id;
        if (ent.is_dir || ent.name[0] == '.' || ent.size < 1024 || !isAudioFile(ent.name, id) || id.empty()) {
            return true;
        }
        ++c->seen;
        auto tracks = c->self->_db.tracks();
        TrackDoc doc;
        const bool thumb = thumbnailExists(id.c_str());
        if (!tracks.get(id, doc)) {
            doc.title = titleFromFileName(id);
            doc.artist = "Local Storage";
            doc.added_at = static_cast<uint32_t>(ent.mtime);
            doc.file_size = static_cast<uint32_t>(ent.size);
            doc.thumbnail = thumb;
            tracks.put(id, doc);
        } else if (doc.file_size != ent.size || doc.thumbnail != thumb) {
            doc.file_size = static_cast<uint32_t>(ent.size);
            doc.thumbnail = thumb;
            tracks.merge(id, doc, TrackDoc::F_FILE_SIZE | TrackDoc::F_THUMBNAIL);
        }
        return true;
    }, &ctx);

    _db.db().flush();
    return ctx.seen;
}

bool CatalogDB::thumbnailExists(const char* videoId) {
    char path[128];
    snprintf(path, sizeof(path), "%s/%s.jpg", THUMBS_DIR, videoId);
    sd_storage::PathInfo info;
    return Fs::stat(path, info) && info.size > 0;
}
