#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#pragma pack(push, 1)

// Track status flags
#define TRACK_FLAG_VALID          (1 << 0)  // Record is active
#define TRACK_FLAG_HAS_SEEK_TABLE (1 << 1)  // Has at least one seek entry
#define TRACK_FLAG_PINNED         (1 << 2)  // Protected from auto-eviction
#define TRACK_FLAG_SEEK_COMPLETE  (1 << 3)  // Complete seek table generated
#define TRACK_FLAG_HAS_THUMBNAIL  (1 << 4)  // Local /sdcard/music/thumbs/<id>.jpg exists

struct SeekEntry {
    uint32_t timecodeMs;    // Playback time in milliseconds from track start
    uint32_t byteOffset;    // Stream/file byte position corresponding to this keyframe
};

struct TrackRecord {
    // ── 1. Header (16 bytes) ─────────────────────────────────────────────
    uint32_t magic;             // 0xCAFEBEEF sentinel
    uint16_t version;           // Schema version (currently 2)
    uint8_t  flags;             // TRACK_FLAG_* bitmask
    uint8_t  _pad0[9];

    // ── 2. Track Identity (160 bytes) ───────────────────────────────────
    char videoId[32];           // Invidious / YouTube video ID (null-terminated)
    char title[64];             // Display title (UTF-8, null-terminated)
    char artist[32];            // Artist / channel name (null-terminated)
    char album[32];             // Album or playlist name (null-terminated)

    // ── 3. Playback Metadata (32 bytes) ─────────────────────────────────
    uint32_t durationMs;        // Total track duration in milliseconds
    uint32_t fileSizeBytes;     // Size of cached .webm/.opus file (0 = stream only)
    uint32_t avgBitrateBps;     // Average bitrate (fallback seek estimation)
    uint32_t cachedAt;          // Unix epoch timestamp of local download
    uint32_t lastPlayedAt;      // Unix epoch timestamp of last playback
    uint32_t playCount;         // Lifetime play count
    uint32_t streamDurationMs;  // Duration reported by online metadata
    uint32_t _pad1;

    // ── 4. Audio Properties (8 bytes) ───────────────────────────────────
    uint32_t sampleRate;        // Typically 48000 for Opus/WebM
    uint16_t channels;          // 1 = mono, 2 = stereo
    uint16_t codecId;           // 0 = WebM/Opus, 1 = PCM/WAV, 2 = Ogg/Opus

    // ── 5. Inline Seek Table (804 bytes) ─────────────────────────────────
    // Stores up to 100 keyframes inline. At 10s intervals, covers 16.6 minutes.
    uint16_t seekEntryCount;    // Populated entries in seekTable[] (max 100)
    uint16_t _reservedSeek;
    SeekEntry seekTable[100];   // 100 × 8 bytes = 800 bytes

    // ── 6. Sector Padding (4 bytes) ──────────────────────────────────────
    uint8_t  _reserved[4];      // Total: 16 + 160 + 32 + 8 + 804 + 4 = 1024 bytes
};

static_assert(sizeof(TrackRecord) == 1024, "TrackRecord must be exactly 1024 bytes (2 FAT32 sectors)");

// In-memory PSRAM Index Entry (24 bytes)
struct IdxEntry {
    uint32_t hash;          // FNV-1a 32-bit hash of videoId (0 = empty slot)
    uint16_t recordNum;     // Record slot index in catalog.db (0 .. N-1)
    uint8_t  flags;         // bit0 = occupied, bit1 = tombstone
    char     videoId[17];   // 16-char null-terminated ID for zero-disk string match
};

static_assert(sizeof(IdxEntry) == 24, "IdxEntry must be exactly 24 bytes");

enum class WalOpType : uint8_t {
    UPSERT_TRACK     = 0x01,
    DELETE_TRACK     = 0x02,
    UPDATE_SEEK_TBL  = 0x03,
    INCREMENT_PLAY   = 0x04
};

struct WalEntry {
    uint32_t  crc32;
    uint32_t  timestamp;
    WalOpType opType;
    uint8_t   _pad[3];
    char      videoId[32];
    TrackRecord record;
};

#pragma pack(pop)

class CatalogDB {
public:
    static CatalogDB& getInstance();

    bool begin();

    // Track CRUD
    bool upsert(const TrackRecord& record);
    bool remove(const char* videoId);
    bool get(const char* videoId, TrackRecord& out);
    bool exists(const char* videoId);

    // Queries
    std::vector<TrackRecord> getAll();
    std::vector<TrackRecord> search(const char* query);
    size_t getTrackCount() const;

    // Seek table
    bool setSeekTable(const char* videoId, const SeekEntry* entries, uint16_t count);
    bool lookupSeekEntry(const char* videoId, uint32_t targetMs,
                         uint32_t& outTimecodeMs, uint32_t& outByteOffset);

    // Thumbnails & Metadata
    bool setThumbnailCached(const char* videoId, bool cached);
    void recordPlay(const char* videoId);

    // Filesystem sync. The dashboard reads catalog.db directly (raw records).
    size_t scanAndSync();

    // Maintenance & Migration
    bool compact();
    void buildIndex();
    bool migrateFromLibraryJson();

    static constexpr const char* MUSIC_DIR    = "/sdcard/music";
    static constexpr const char* THUMBS_DIR   = "/sdcard/music/thumbs";
    static constexpr const char* DB_PATH      = "/sdcard/music/catalog.db";
    static constexpr const char* WAL_PATH     = "/sdcard/music/catalog.wal";
    static constexpr const char* LEGACY_JSON  = "/sdcard/music/library.json";
    static constexpr uint32_t    MAGIC_SENTINEL = 0xCAFEBEEF;

private:
    CatalogDB();
    ~CatalogDB();
    CatalogDB(const CatalogDB&) = delete;
    CatalogDB& operator=(const CatalogDB&) = delete;

    SemaphoreHandle_t _mutex = nullptr;
    IdxEntry*         _index = nullptr;  // Allocated in PSRAM
    uint16_t          _indexCap = 1024;  // Power of 2
    uint16_t          _indexCount = 0;   // Valid tracks
    uint16_t          _recordCount = 0;  // Record slots in catalog.db, valid or not
    std::vector<uint16_t> _freeRecords;  // Empty slots below _recordCount, reused first
    bool              _initialized = false;

    // Internal helpers
    uint32_t hashVideoId(const char* id);
    int16_t  findSlot(const char* videoId, uint32_t hash) const;
    int16_t  findInsertSlot(const char* videoId, uint32_t hash) const;
    bool     walAppend(WalOpType op, const TrackRecord& rec);
    uint16_t allocRecord();
    bool     replayWal();
    bool     readRecord(uint16_t recordNum, TrackRecord& out);
    bool     writeRecord(uint16_t recordNum, const TrackRecord& rec);
    static std::string cleanTitleFromFilename(const std::string& filename);
};
