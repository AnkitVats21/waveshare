#include "media_player/CatalogDB.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include <sys/stat.h>
#include <sys/unistd.h>
#include <dirent.h>
#include <cstring>
#include <algorithm>
#include <cerrno>
#include <ctime>
#include <memory>
#include "cJSON.h"

static const char* TAG = "CatalogDB";

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

    mkdir(MUSIC_DIR, 0777);
    mkdir(THUMBS_DIR, 0777);

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

    FILE* f = fopen(DB_PATH, "rb");
    if (!f) return;

    auto rec = std::make_unique<TrackRecord>();
    uint16_t recordNum = 0;

    while (fread(rec.get(), sizeof(TrackRecord), 1, f) == 1) {
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

    _recordCount = recordNum;
    fclose(f);
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
    FILE* f = fopen(DB_PATH, "rb");
    if (!f) return false;

    if (fseek(f, static_cast<long>(recordNum) * sizeof(TrackRecord), SEEK_SET) != 0) {
        fclose(f);
        return false;
    }

    size_t r = fread(&out, sizeof(TrackRecord), 1, f);
    fclose(f);
    return (r == 1 && out.magic == MAGIC_SENTINEL && (out.flags & TRACK_FLAG_VALID));
}

bool CatalogDB::writeRecord(uint16_t recordNum, const TrackRecord& rec) {
    // "w+b" truncates, so it is only for creating the file. Falling back to it
    // on any open failure (e.g. out of file handles) wiped the catalog.
    FILE* f = fopen(DB_PATH, "r+b");
    if (!f && errno == ENOENT) {
        f = fopen(DB_PATH, "w+b");
    }
    if (!f) {
        ESP_LOGW(TAG, "writeRecord %u: open failed (errno %d)", recordNum, errno);
        return false;
    }

    if (fseek(f, static_cast<long>(recordNum) * sizeof(TrackRecord), SEEK_SET) != 0) {
        ESP_LOGW(TAG, "writeRecord %u: seek failed (errno %d)", recordNum, errno);
        fclose(f);
        return false;
    }

    size_t w = fwrite(&rec, sizeof(TrackRecord), 1, f);
    bool ok = (w == 1) && fflush(f) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok) ESP_LOGW(TAG, "writeRecord %u: write failed (errno %d)", recordNum, errno);
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

    FILE* f = fopen(WAL_PATH, "ab");
    if (!f) return false;

    size_t w = fwrite(entry.get(), sizeof(WalEntry), 1, f);
    fflush(f);
    fclose(f);
    return (w == 1);
}

bool CatalogDB::replayWal() {
    FILE* f = fopen(WAL_PATH, "rb");
    if (!f) return false;

    auto entry = std::make_unique<WalEntry>();
    bool replayedAny = false;

    while (fread(entry.get(), sizeof(WalEntry), 1, f) == 1) {
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

    fclose(f);
    unlink(WAL_PATH); // Truncate WAL after replay
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
    struct stat st;
    if (stat(thumbPath, &st) == 0 && st.st_size > 0) {
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
        unlink(WAL_PATH);
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

    unlink(WAL_PATH);

    // Remove local audio files and thumbnail
    const char* exts[] = { ".webm", ".opus", ".ogg" };
    for (const char* ext : exts) {
        char p[128];
        snprintf(p, sizeof(p), "%s/%s%s", MUSIC_DIR, videoId, ext);
        unlink(p);
    }
    char thumbP[128];
    snprintf(thumbP, sizeof(thumbP), "%s/%s.jpg", THUMBS_DIR, videoId);
    unlink(thumbP);

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

    FILE* f = fopen(DB_PATH, "rb");
    if (!f) {
        ESP_LOGW(TAG, "getAll: open %s failed (errno %d)", DB_PATH, errno);
        xSemaphoreGiveRecursive(_mutex);
        return result;
    }

    size_t chunk_size = 4096;
    uint8_t* chunk = static_cast<uint8_t*>(heap_caps_malloc(chunk_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    if (!chunk) {
        chunk_size = sizeof(TrackRecord);
        chunk = static_cast<uint8_t*>(heap_caps_malloc(chunk_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    }
    if (!chunk) {
        ESP_LOGW(TAG, "getAll: no internal RAM for the read buffer");
        fclose(f);
        xSemaphoreGiveRecursive(_mutex);
        return result;
    }
    result.reserve(_indexCount);
    size_t n;
    while ((n = fread(chunk, 1, chunk_size, f)) >= sizeof(TrackRecord)) {
        for (size_t off = 0; off + sizeof(TrackRecord) <= n; off += sizeof(TrackRecord)) {
            const auto* rec = reinterpret_cast<const TrackRecord*>(chunk + off);
            if (rec->magic == MAGIC_SENTINEL && (rec->flags & TRACK_FLAG_VALID)) {
                result.push_back(*rec);
            }
        }
    }
    heap_caps_free(chunk);

    if (ferror(f)) {
        ESP_LOGW(TAG, "getAll: read error after %u records (errno %d)", (unsigned)result.size(), errno);
    }
    fclose(f);
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

bool CatalogDB::setSeekTable(const char* videoId, const SeekEntry* entries, uint16_t count) {
    if (!videoId || !entries || count == 0) return false;

    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);

    auto rec = std::make_unique<TrackRecord>();
    if (!get(videoId, *rec)) {
        xSemaphoreGiveRecursive(_mutex);
        return false;
    }

    uint16_t toCopy = std::min(count, static_cast<uint16_t>(100));
    memcpy(rec->seekTable, entries, toCopy * sizeof(SeekEntry));
    rec->seekEntryCount = toCopy;
    rec->flags |= (TRACK_FLAG_HAS_SEEK_TABLE | TRACK_FLAG_SEEK_COMPLETE);

    bool ok = upsert(*rec);
    xSemaphoreGiveRecursive(_mutex);
    return ok;
}

bool CatalogDB::lookupSeekEntry(const char* videoId, uint32_t targetMs,
                                uint32_t& outTimecodeMs, uint32_t& outByteOffset) {
    if (!videoId) return false;

    auto rec = std::make_unique<TrackRecord>();
    if (!get(videoId, *rec)) return false;

    // 1. Check if populated seek table exists
    if ((rec->flags & TRACK_FLAG_HAS_SEEK_TABLE) && rec->seekEntryCount > 0) {
        // Nearest keyframe <= targetMs
        uint32_t bestTime = 0;
        uint32_t bestOffset = 0;
        for (uint16_t i = 0; i < rec->seekEntryCount; ++i) {
            if (rec->seekTable[i].timecodeMs <= targetMs) {
                bestTime = rec->seekTable[i].timecodeMs;
                bestOffset = rec->seekTable[i].byteOffset;
            } else {
                break;
            }
        }
        outTimecodeMs = bestTime;
        outByteOffset = bestOffset;
        return true;
    }

    // 2. Fallback estimation via duration and file size
    if (rec->durationMs > 0 && rec->fileSizeBytes > 0) {
        uint64_t est = (static_cast<uint64_t>(targetMs) * rec->fileSizeBytes) / rec->durationMs;
        outTimecodeMs = targetMs;
        outByteOffset = static_cast<uint32_t>(est);
        return true;
    }

    // 3. Fallback estimation via bitrate
    if (rec->avgBitrateBps > 0) {
        uint64_t est = (static_cast<uint64_t>(targetMs) * rec->avgBitrateBps) / 8000;
        outTimecodeMs = targetMs;
        outByteOffset = static_cast<uint32_t>(est);
        return true;
    }

    return false;
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

    DIR* dir = opendir(MUSIC_DIR);
    if (!dir) {
        xSemaphoreGiveRecursive(_mutex);
        return 0;
    }

    struct dirent* ent;
    size_t indexed = 0;

    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string fname = ent->d_name;

        bool isWebm = (fname.size() > 5 && fname.substr(fname.size() - 5) == ".webm");
        bool isOpus = (fname.size() > 5 && fname.substr(fname.size() - 5) == ".opus");
        bool isOgg  = (fname.size() > 4 && fname.substr(fname.size() - 4) == ".ogg");

        if (!isWebm && !isOpus && !isOgg) continue;

        size_t lastDot = fname.find_last_of('.');
        std::string baseId = fname.substr(0, lastDot);
        if (baseId.empty()) continue;

        std::string fullPath = std::string(MUSIC_DIR) + "/" + fname;
        struct stat st;
        if (stat(fullPath.c_str(), &st) != 0 || st.st_size < 1024) continue;

        auto rec = std::make_unique<TrackRecord>();
        bool existing = get(baseId.c_str(), *rec);

        if (!existing) {
            strncpy(rec->videoId, baseId.c_str(), sizeof(rec->videoId) - 1);
            std::string cleanTitle = cleanTitleFromFilename(fname);
            strncpy(rec->title, cleanTitle.c_str(), sizeof(rec->title) - 1);
            strncpy(rec->artist, "Local Storage", sizeof(rec->artist) - 1);
            rec->sampleRate = 48000;
            rec->channels = 2;
            rec->codecId = isWebm ? 0 : 2;
            rec->cachedAt = static_cast<uint32_t>(st.st_mtime);
        }

        rec->fileSizeBytes = static_cast<uint32_t>(st.st_size);

        // Check thumbnail
        char thumbPath[128];
        snprintf(thumbPath, sizeof(thumbPath), "%s/%s.jpg", THUMBS_DIR, baseId.c_str());
        struct stat thumbSt;
        if (stat(thumbPath, &thumbSt) == 0 && thumbSt.st_size > 0) {
            rec->flags |= TRACK_FLAG_HAS_THUMBNAIL;
        }

        upsert(*rec);
        indexed++;
    }

    closedir(dir);
    xSemaphoreGiveRecursive(_mutex);
    return indexed;
}

bool CatalogDB::compact() {
    xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);

    std::string tempPath = std::string(DB_PATH) + ".tmp";
    FILE* src = fopen(DB_PATH, "rb");
    if (!src) {
        xSemaphoreGiveRecursive(_mutex);
        return false;
    }

    FILE* dst = fopen(tempPath.c_str(), "wb");
    if (!dst) {
        fclose(src);
        xSemaphoreGiveRecursive(_mutex);
        return false;
    }

    auto rec = std::make_unique<TrackRecord>();
    while (fread(rec.get(), sizeof(TrackRecord), 1, src) == 1) {
        if (rec->magic == MAGIC_SENTINEL && (rec->flags & TRACK_FLAG_VALID)) {
            fwrite(rec.get(), sizeof(TrackRecord), 1, dst);
        }
    }

    fclose(src);
    fclose(dst);

    unlink(DB_PATH);
    rename(tempPath.c_str(), DB_PATH);

    buildIndex();
    xSemaphoreGiveRecursive(_mutex);
    return true;
}

bool CatalogDB::migrateFromLibraryJson() {
    struct stat st;
    if (stat(LEGACY_JSON, &st) != 0 || st.st_size == 0) {
        return false; // No library.json to migrate
    }

    if (stat(DB_PATH, &st) == 0 && st.st_size >= sizeof(TrackRecord)) {
        return false; // catalog.db already exists
    }

    ESP_LOGI(TAG, "Migrating legacy library.json to catalog.db (%ld bytes)", (long)st.st_size);

    FILE* f = fopen(LEGACY_JSON, "rb");
    if (!f) return false;

    std::string content(st.st_size, '\0');
    fread(&content[0], 1, st.st_size, f);
    fclose(f);

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
    rename(LEGACY_JSON, bak.c_str());
    ESP_LOGI(TAG, "Migrated %d tracks to catalog.db; renamed library.json to %s", count, bak.c_str());
    return true;
}
