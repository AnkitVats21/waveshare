#include "media_player/CatalogDB.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "sd_storage/File.h"
#include "sd_storage/Fs.h"
#include <cstring>
#include <algorithm>
#include <ctime>
#include <memory>
#include "cJSON.h"

static const char* TAG = "CatalogDB";

using sd_storage::File;
using sd_storage::Mode;
namespace Fs = sd_storage::Fs;

namespace {
constexpr size_t kScanChunk = 4 * sizeof(TrackRecord);

struct CapsFree { void operator()(void* p) const { heap_caps_free(p); } };

// Whole-file scans read several records per call into PSRAM.
std::unique_ptr<uint8_t, CapsFree> scanBuffer() {
    return std::unique_ptr<uint8_t, CapsFree>(
        static_cast<uint8_t*>(heap_caps_malloc(kScanChunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
}
} // namespace

CatalogDB& CatalogDB::getInstance() {
    static CatalogDB instance;
    return instance;
}

CatalogDB::CatalogDB() {
    _mutex = xSemaphoreCreateRecursiveMutex();
}

CatalogDB::~CatalogDB() {
    if (_index) {
        free(_index);
        _index = nullptr;
    }
    if (_mutex) {
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
    }
}

uint32_t CatalogDB::hashVideoId(const char* id) {
    if (!id || id[0] == '\0') return 0;
    uint32_t hash = 2166136261u;
    for (const char* p = id; *p; ++p) {
        hash ^= static_cast<uint8_t>(*p);
        hash *= 16777619u;
    }
    return (hash == 0) ? 1 : hash;
}

int16_t CatalogDB::findSlot(const char* videoId, uint32_t hash) const {
    if (!_index || _indexCap == 0 || !videoId) return -1;
    uint16_t mask = _indexCap - 1;
    uint16_t slot = hash & mask;

    for (uint16_t i = 0; i < _indexCap; ++i) {
        const IdxEntry& e = _index[slot];
        if (e.hash == 0 && (e.flags & 0x01) == 0 && (e.flags & 0x02) == 0) {
            return -1; // Empty slot (never used)
        }
        if ((e.flags & 0x01) && e.hash == hash && strncmp(e.videoId, videoId, sizeof(e.videoId)) == 0) {
            return slot;
        }
        slot = (slot + 1) & mask;
    }
    return -1;
}

int16_t CatalogDB::findInsertSlot(const char* videoId, uint32_t hash) const {
    if (!_index || _indexCap == 0 || !videoId) return -1;
    uint16_t mask = _indexCap - 1;
    uint16_t slot = hash & mask;
    int16_t firstTombstone = -1;

    for (uint16_t i = 0; i < _indexCap; ++i) {
        const IdxEntry& e = _index[slot];
        if ((e.flags & 0x01) && e.hash == hash && strncmp(e.videoId, videoId, sizeof(e.videoId)) == 0) {
            return slot; // Overwrite existing slot
        }
        if ((e.flags & 0x02) && firstTombstone == -1) {
            firstTombstone = slot;
        }
        if (e.hash == 0 && (e.flags & 0x01) == 0 && (e.flags & 0x02) == 0) {
            return (firstTombstone != -1) ? firstTombstone : slot;
        }
        slot = (slot + 1) & mask;
    }
    return firstTombstone;
}

bool CatalogDB::begin() {
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);

    if (_initialized) {
        xSemaphoreGiveRecursive(_mutex);
        return true;
    }

    Fs::mkdirs(THUMBS_DIR);  // and MUSIC_DIR above it

    // Allocate PSRAM index (1024 slots * 24B = 24.5 KB)
    _indexCap = 1024;
    _index = static_cast<IdxEntry*>(heap_caps_calloc(_indexCap, sizeof(IdxEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!_index) {
        _index = static_cast<IdxEntry*>(calloc(_indexCap, sizeof(IdxEntry)));
    }
    if (!_index) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM catalog index");
        xSemaphoreGiveRecursive(_mutex);
        return false;
    }

    // The index must exist before the migration and WAL replay below: both
    // look tracks up and pick record slots through it.
    buildIndex();

    // Check for legacy library.json migration
    migrateFromLibraryJson();

    // Replay WAL if it contains uncommitted transactions
    if (replayWal()) buildIndex();

    _initialized = true;
    ESP_LOGI(TAG, "CatalogDB initialized with %u tracks in PSRAM index", _indexCount);
    xSemaphoreGiveRecursive(_mutex);
    return true;
}

void CatalogDB::buildIndex() {
    _indexCount = 0;
    _recordCount = 0;
    _freeRecords.clear();
    memset(_index, 0, _indexCap * sizeof(IdxEntry));

    File f = File::open(DB_PATH, Mode::Read);
    if (!f) return;
    auto buf = scanBuffer();
    if (!buf) {
        ESP_LOGE(TAG, "buildIndex: out of PSRAM");
        return;
    }

    uint16_t recordNum = 0;
    size_t n;
    while ((n = f.read(buf.get(), kScanChunk)) >= sizeof(TrackRecord)) {
      for (size_t off = 0; off + sizeof(TrackRecord) <= n; off += sizeof(TrackRecord)) {
        const auto* rec = reinterpret_cast<const TrackRecord*>(buf.get() + off);
        if (rec->magic == MAGIC_SENTINEL && (rec->flags & TRACK_FLAG_VALID) && rec->videoId[0] != '\0') {
            uint32_t h = hashVideoId(rec->videoId);
            int16_t slot = findInsertSlot(rec->videoId, h);
            if (slot >= 0) {
                _index[slot].hash = h;
                _index[slot].recordNum = recordNum;
                _index[slot].flags = 0x01; // Occupied
                strncpy(_index[slot].videoId, rec->videoId, sizeof(_index[slot].videoId) - 1);
                _index[slot].videoId[sizeof(_index[slot].videoId) - 1] = '\0';
                _indexCount++;
            }
        } else {
            _freeRecords.push_back(recordNum);
        }
        recordNum++;
      }
    }

    _recordCount = recordNum;
}

// A slot for a new track: a hole left by a removed one, else the end of the
// file. (Using the valid-track count here overwrote live records once the
// file had holes.)
uint16_t CatalogDB::allocRecord() {
    if (!_freeRecords.empty()) {
        uint16_t rec = _freeRecords.back();
        _freeRecords.pop_back();
        return rec;
    }
    return _recordCount++;
}

bool CatalogDB::readRecord(uint16_t recordNum, TrackRecord& out) {
    File f = File::open(DB_PATH, Mode::Read);
    if (!f || !f.seek(static_cast<long>(recordNum) * sizeof(TrackRecord))) return false;
    return f.readExact(&out, sizeof(TrackRecord)) && out.magic == MAGIC_SENTINEL && (out.flags & TRACK_FLAG_VALID);
}

bool CatalogDB::writeRecord(uint16_t recordNum, const TrackRecord& rec) {
    // Never truncates: an earlier "w+b" fallback on any open failure wiped
    // the catalog.
    File f = File::open(DB_PATH, Mode::UpdateOrCreate);
    if (!f) {
        ESP_LOGW(TAG, "writeRecord %u: open failed", recordNum);
        return false;
    }
    bool ok = f.seek(static_cast<long>(recordNum) * sizeof(TrackRecord)) &&
              f.writeAll(&rec, sizeof(TrackRecord)) && f.sync();
    if (!ok) ESP_LOGW(TAG, "writeRecord %u: write failed", recordNum);
    return ok;
}

bool CatalogDB::walAppend(WalOpType op, const TrackRecord& rec) {
    auto entry = std::make_unique<WalEntry>();
    entry->timestamp = static_cast<uint32_t>(time(nullptr));
    entry->opType = op;
    strncpy(entry->videoId, rec.videoId, sizeof(entry->videoId) - 1);
    entry->record = rec;

    // CRC of entry excluding the crc32 field
    const uint8_t* p = reinterpret_cast<const uint8_t*>(entry.get()) + sizeof(uint32_t);
    size_t len = sizeof(WalEntry) - sizeof(uint32_t);
    entry->crc32 = esp_rom_crc32_le(0, p, len);

    return Fs::append(WAL_PATH, entry.get(), sizeof(WalEntry));
}

bool CatalogDB::replayWal() {
    File f = File::open(WAL_PATH, Mode::Read);
    if (!f) return false;

    auto entry = std::make_unique<WalEntry>();
    bool replayedAny = false;

    while (f.readExact(entry.get(), sizeof(WalEntry))) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(entry.get()) + sizeof(uint32_t);
        size_t len = sizeof(WalEntry) - sizeof(uint32_t);
        uint32_t expectedCrc = esp_rom_crc32_le(0, p, len);

        if (entry->crc32 == expectedCrc) {
            if (entry->opType == WalOpType::UPSERT_TRACK || entry->opType == WalOpType::UPDATE_SEEK_TBL) {
                uint32_t h = hashVideoId(entry->videoId);
                int16_t slot = findSlot(entry->videoId, h);
                uint16_t recNum = (slot >= 0) ? _index[slot].recordNum : allocRecord();
                writeRecord(recNum, entry->record);
                replayedAny = true;
            } else if (entry->opType == WalOpType::DELETE_TRACK) {
                uint32_t h = hashVideoId(entry->videoId);
                int16_t slot = findSlot(entry->videoId, h);
                if (slot >= 0) {
                    auto emptyRec = std::make_unique<TrackRecord>();
                    writeRecord(_index[slot].recordNum, *emptyRec);
                    replayedAny = true;
                }
            }
        }
    }

    f.close();
    Fs::remove(WAL_PATH); // Truncate WAL after replay
    return replayedAny;
}

bool CatalogDB::upsert(const TrackRecord& record) {
    if (record.videoId[0] == '\0') return false;

    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);

    uint32_t h = hashVideoId(record.videoId);
    int16_t slot = findSlot(record.videoId, h);
    uint16_t targetRecNum = 0;
    const bool existed = slot >= 0;

    if (slot >= 0) {
        targetRecNum = _index[slot].recordNum;
    } else {
        // Find end of file or first reusable slot
        slot = findInsertSlot(record.videoId, h);
        if (slot < 0) {
            xSemaphoreGiveRecursive(_mutex);
            ESP_LOGE(TAG, "PSRAM index full");
            return false;
        }
        targetRecNum = allocRecord();
    }

    auto toWrite = std::make_unique<TrackRecord>(record);
    toWrite->magic = MAGIC_SENTINEL;
    toWrite->version = 2;
    toWrite->flags |= TRACK_FLAG_VALID;

    // Check thumbnail presence
    char thumbPath[128];
    snprintf(thumbPath, sizeof(thumbPath), "%s/%s.jpg", THUMBS_DIR, record.videoId);
    sd_storage::PathInfo thumb;
    if (Fs::stat(thumbPath, thumb) && thumb.size > 0) {
        toWrite->flags |= TRACK_FLAG_HAS_THUMBNAIL;
    }

    walAppend(WalOpType::UPSERT_TRACK, *toWrite);
    bool ok = writeRecord(targetRecNum, *toWrite);
    if (!ok && !existed) _freeRecords.push_back(targetRecNum);

    if (ok) {
        _index[slot].hash = h;
        _index[slot].recordNum = targetRecNum;
        _index[slot].flags = 0x01;
        strncpy(_index[slot].videoId, record.videoId, sizeof(_index[slot].videoId) - 1);
        _index[slot].videoId[sizeof(_index[slot].videoId) - 1] = '\0';
        if (!existed) _indexCount++;
        Fs::remove(WAL_PATH);
    }

    xSemaphoreGiveRecursive(_mutex);
    return ok;
}

bool CatalogDB::remove(const char* videoId) {
    if (!videoId || videoId[0] == '\0') return false;

    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);

    uint32_t h = hashVideoId(videoId);
    int16_t slot = findSlot(videoId, h);
    if (slot < 0) {
        xSemaphoreGiveRecursive(_mutex);
        return false;
    }

    uint16_t recNum = _index[slot].recordNum;
    auto emptyRec = std::make_unique<TrackRecord>();
    // The id lets a WAL replay find the record; without TRACK_FLAG_VALID the
    // slot still reads as empty.
    strncpy(emptyRec->videoId, videoId, sizeof(emptyRec->videoId) - 1);
    walAppend(WalOpType::DELETE_TRACK, *emptyRec);
    writeRecord(recNum, *emptyRec);
    _freeRecords.push_back(recNum);

    // Mark slot as tombstone
    _index[slot].flags = 0x02;
    _index[slot].hash = 0;
    _index[slot].videoId[0] = '\0';
    if (_indexCount > 0) _indexCount--;

    Fs::remove(WAL_PATH);

    // Remove local audio files and thumbnail
    const char* exts[] = { ".webm", ".opus", ".ogg" };
    for (const char* ext : exts) {
        char p[128];
        snprintf(p, sizeof(p), "%s/%s%s", MUSIC_DIR, videoId, ext);
        Fs::remove(p);
    }
    char thumbP[128];
    snprintf(thumbP, sizeof(thumbP), "%s/%s.jpg", THUMBS_DIR, videoId);
    Fs::remove(thumbP);

    xSemaphoreGiveRecursive(_mutex);
    return true;
}

bool CatalogDB::get(const char* videoId, TrackRecord& out) {
    if (!videoId || videoId[0] == '\0') return false;

    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);

    uint32_t h = hashVideoId(videoId);
    int16_t slot = findSlot(videoId, h);
    if (slot < 0) {
        xSemaphoreGiveRecursive(_mutex);
        return false;
    }

    bool ok = readRecord(_index[slot].recordNum, out);
    xSemaphoreGiveRecursive(_mutex);
    return ok;
}

bool CatalogDB::exists(const char* videoId) {
    if (!videoId || videoId[0] == '\0') return false;
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    uint32_t h = hashVideoId(videoId);
    int16_t slot = findSlot(videoId, h);
    xSemaphoreGiveRecursive(_mutex);
    return (slot >= 0);
}

std::vector<TrackRecord> CatalogDB::getAll() {
    std::vector<TrackRecord> result;
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);

    File f = File::open(DB_PATH, Mode::Read);
    auto buf = scanBuffer();
    if (!f || !buf) {
        ESP_LOGW(TAG, "getAll: cannot read %s", DB_PATH);
        xSemaphoreGiveRecursive(_mutex);
        return result;
    }
    result.reserve(_indexCount);
    size_t n;
    while ((n = f.read(buf.get(), kScanChunk)) >= sizeof(TrackRecord)) {
        for (size_t off = 0; off + sizeof(TrackRecord) <= n; off += sizeof(TrackRecord)) {
            const auto* rec = reinterpret_cast<const TrackRecord*>(buf.get() + off);
            if (rec->magic == MAGIC_SENTINEL && (rec->flags & TRACK_FLAG_VALID)) {
                result.push_back(*rec);
            }
        }
    }
    xSemaphoreGiveRecursive(_mutex);
    return result;
}

static bool containsIgnoreCase(const std::string& str, const std::string& sub) {
    if (sub.empty()) return true;
    auto it = std::search(
        str.begin(), str.end(),
        sub.begin(), sub.end(),
        [](char ch1, char ch2) { return std::tolower(ch1) == std::tolower(ch2); }
    );
    return (it != str.end());
}

std::vector<TrackRecord> CatalogDB::search(const char* query) {
    std::vector<TrackRecord> result;
    if (!query) return result;
    std::string q = query;

    auto all = getAll();
    for (const auto& r : all) {
        if (containsIgnoreCase(r.title, q) || containsIgnoreCase(r.artist, q) || containsIgnoreCase(r.videoId, q)) {
            result.push_back(r);
        }
    }
    return result;
}

size_t CatalogDB::getTrackCount() const {
    return _indexCount;
}

bool CatalogDB::setThumbnailCached(const char* videoId, bool cached) {
    auto rec = std::make_unique<TrackRecord>();
    if (!get(videoId, *rec)) return false;
    if (cached) {
        rec->flags |= TRACK_FLAG_HAS_THUMBNAIL;
    } else {
        rec->flags &= ~TRACK_FLAG_HAS_THUMBNAIL;
    }
    return upsert(*rec);
}

void CatalogDB::recordPlay(const char* videoId) {
    auto rec = std::make_unique<TrackRecord>();
    if (get(videoId, *rec)) {
        rec->playCount++;
        rec->lastPlayedAt = static_cast<uint32_t>(time(nullptr));
        upsert(*rec);
    }
}

std::string CatalogDB::cleanTitleFromFilename(const std::string& filename) {
    std::string title = filename;
    size_t lastDot = title.find_last_of('.');
    if (lastDot != std::string::npos) {
        title = title.substr(0, lastDot);
    }
    for (char& c : title) {
        if (c == '_' || c == '-') c = ' ';
    }
    return title;
}

size_t CatalogDB::scanAndSync() {
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);

    struct ScanCtx {
        CatalogDB* db;
        size_t indexed;
    } ctx{this, 0};

    Fs::list(MUSIC_DIR, nullptr, true, [](const sd_storage::DirEntry& ent, void* p) {
        auto* c = static_cast<ScanCtx*>(p);
        if (ent.is_dir || ent.name[0] == '.' || ent.size < 1024) return true;
        std::string fname = ent.name;

        bool isWebm = (fname.size() > 5 && fname.substr(fname.size() - 5) == ".webm");
        bool isOpus = (fname.size() > 5 && fname.substr(fname.size() - 5) == ".opus");
        bool isOgg  = (fname.size() > 4 && fname.substr(fname.size() - 4) == ".ogg");
        if (!isWebm && !isOpus && !isOgg) return true;

        size_t lastDot = fname.find_last_of('.');
        std::string baseId = fname.substr(0, lastDot);
        if (baseId.empty()) return true;

        auto rec = std::make_unique<TrackRecord>();
        bool existing = c->db->get(baseId.c_str(), *rec);

        if (!existing) {
            strncpy(rec->videoId, baseId.c_str(), sizeof(rec->videoId) - 1);
            std::string cleanTitle = cleanTitleFromFilename(fname);
            strncpy(rec->title, cleanTitle.c_str(), sizeof(rec->title) - 1);
            strncpy(rec->artist, "Local Storage", sizeof(rec->artist) - 1);
            rec->sampleRate = 48000;
            rec->channels = 2;
            rec->codecId = isWebm ? 0 : 2;
            rec->cachedAt = static_cast<uint32_t>(ent.mtime);
        }

        rec->fileSizeBytes = static_cast<uint32_t>(ent.size);

        // Check thumbnail
        char thumbPath[128];
        snprintf(thumbPath, sizeof(thumbPath), "%s/%s.jpg", THUMBS_DIR, baseId.c_str());
        sd_storage::PathInfo thumb;
        if (Fs::stat(thumbPath, thumb) && thumb.size > 0) {
            rec->flags |= TRACK_FLAG_HAS_THUMBNAIL;
        }

        c->db->upsert(*rec);
        c->indexed++;
        return true;
    }, &ctx);

    xSemaphoreGiveRecursive(_mutex);
    return ctx.indexed;
}

bool CatalogDB::compact() {
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);

    std::string tempPath = std::string(DB_PATH) + ".tmp";
    bool ok = false;
    {
        File src = File::open(DB_PATH, Mode::Read);
        File dst = src ? File::open(tempPath.c_str(), Mode::Write) : File();
        auto rec = std::make_unique<TrackRecord>();
        ok = src && dst;
        while (ok && src.readExact(rec.get(), sizeof(TrackRecord))) {
            if (rec->magic == MAGIC_SENTINEL && (rec->flags & TRACK_FLAG_VALID)) {
                ok = dst.writeAll(rec.get(), sizeof(TrackRecord));
            }
        }
        ok = ok && dst.sync();
    }
    // Swap in the compacted file only if it was fully written and the old one
    // could be removed (not open elsewhere, e.g. a dashboard download).
    ok = ok && Fs::remove(DB_PATH) && Fs::rename(tempPath.c_str(), DB_PATH);
    if (!ok) {
        ESP_LOGW(TAG, "compact failed; catalog left as it was");
        Fs::remove(tempPath.c_str());
    }

    buildIndex();
    xSemaphoreGiveRecursive(_mutex);
    return ok;
}

bool CatalogDB::migrateFromLibraryJson() {
    sd_storage::PathInfo legacy, db;
    if (!Fs::stat(LEGACY_JSON, legacy) || legacy.size == 0) {
        return false; // No library.json to migrate
    }

    if (Fs::stat(DB_PATH, db) && db.size >= sizeof(TrackRecord)) {
        return false; // catalog.db already exists
    }

    ESP_LOGI(TAG, "Migrating legacy library.json to catalog.db (%ld bytes)", (long)legacy.size);

    std::string content = Fs::readText(LEGACY_JSON, 1024 * 1024);
    if (content.empty()) return false;

    cJSON* root = cJSON_Parse(content.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse library.json for migration");
        return false;
    }

    int count = cJSON_GetArraySize(root);
    for (int i = 0; i < count; ++i) {
        cJSON* item = cJSON_GetArrayItem(root, i);
        if (!item) continue;

        cJSON* jId = cJSON_GetObjectItem(item, "id");
        if (!jId || !jId->valuestring) continue;

        auto rec = std::make_unique<TrackRecord>();
        strncpy(rec->videoId, jId->valuestring, sizeof(rec->videoId) - 1);

        cJSON* jTitle = cJSON_GetObjectItem(item, "title");
        if (jTitle && jTitle->valuestring) {
            strncpy(rec->title, jTitle->valuestring, sizeof(rec->title) - 1);
        }

        cJSON* jArtist = cJSON_GetObjectItem(item, "artist");
        if (jArtist && jArtist->valuestring) {
            strncpy(rec->artist, jArtist->valuestring, sizeof(rec->artist) - 1);
        }

        cJSON* jDur = cJSON_GetObjectItem(item, "duration");
        if (jDur) {
            rec->durationMs = static_cast<uint32_t>(jDur->valueint * 1000);
        }

        cJSON* jSize = cJSON_GetObjectItem(item, "size");
        if (jSize) {
            rec->fileSizeBytes = static_cast<uint32_t>(jSize->valueint);
        }

        cJSON* jCachedAt = cJSON_GetObjectItem(item, "cached_at");
        if (jCachedAt) {
            rec->cachedAt = static_cast<uint32_t>(jCachedAt->valueint);
        }

        rec->sampleRate = 48000;
        rec->channels = 2;
        rec->codecId = 0; // Default to WebM/Opus

        upsert(*rec);
    }

    cJSON_Delete(root);

    // Rename library.json to library.json.bak
    std::string bak = std::string(LEGACY_JSON) + ".bak";
    Fs::rename(LEGACY_JSON, bak.c_str());
    ESP_LOGI(TAG, "Migrated %d tracks to catalog.db; renamed library.json to %s", count, bak.c_str());
    return true;
}
