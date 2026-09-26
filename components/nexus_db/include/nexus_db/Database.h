#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "nexus_db/Io.h"
#include "nexus_db/PsramAllocator.h"

namespace nexus_db {

enum class Flush : uint8_t {
    EveryCommit,  // fsync after every write
    Batched,      // fsync when the last one is FLUSH_INTERVAL_MS old, or on flush()
};

struct CollectionDef {
    uint8_t id;        // 1..255, stored in each record
    const char* name;
    bool cached;       // documents kept in RAM; required for merge
};

struct Options {
    const char* name;  // stored in the header, at most 11 characters
    const char* path;  // e.g. "/sdcard/db/music.ndb"
    uint32_t schema_hash;
    Flush flush;
    const CollectionDef* collections;
    size_t collection_count;
    // Cleanup runs when dead bytes exceed live bytes and the file is larger
    // than this. Tests lower it.
    uint32_t compact_min_bytes = 64 * 1024;
};

struct Stats {
    uint32_t file_bytes = 0;
    uint32_t live_bytes = 0;     // size of the file right after a cleanup
    uint32_t records = 0;        // valid records read at open plus written since
    uint32_t skipped_bytes = 0;  // corrupt bytes skipped at open
    uint32_t truncated_bytes = 0;// cut-off tail removed at open
    uint32_t generation = 0;
    uint32_t compactions = 0;
    uint32_t documents = 0;
};

// Append-only log of put / merge / delete records with an in-RAM index.
// See docs/nexus-db-design.md for the file format.
//
// Thread safe: every call takes the database mutex. forEach callbacks run
// with it held and must not call back into the database.
class Database {
public:
    static constexpr uint16_t FORMAT_VERSION = 1;
    static constexpr size_t MAX_KEY = 64;
    static constexpr uint32_t FLUSH_INTERVAL_MS = 2000;

    Database(const Options& options, Io& io);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Reads the log and builds the index. Recovers from a cut-off tail, a
    // corrupt region and an interrupted cleanup; see the design doc.
    bool open();
    void close();
    bool isOpen() const;

    // `value` is a sequence of fields (see Writer).
    bool put(uint8_t collection, std::string_view key, std::string_view value);
    // Changes only the given fields. Cached collections only. Creates the
    // document if it doesn't exist.
    bool merge(uint8_t collection, std::string_view key, std::string_view value);
    bool remove(uint8_t collection, std::string_view key);

    // Copies the document's fields into `out`. False if missing.
    bool get(uint8_t collection, std::string_view key, std::string& out);
    bool contains(uint8_t collection, std::string_view key);
    size_t count(uint8_t collection);
    // Calls fn(key, fields) for each document, in no particular order.
    // Return false to stop.
    using Visitor = std::function<bool(std::string_view key, std::string_view value)>;
    void forEach(uint8_t collection, const Visitor& fn);

    // fsync pending writes (Batched mode).
    bool flush();
    // Batched mode: fsync if the oldest unsynced write is due. Call from the
    // owner's periodic task.
    bool flushIfDue();
    // Rewrites the file with live documents only. Runs by itself when the file
    // is mostly dead records; exposed for tests and maintenance.
    bool compact();

    Stats stats();
    const Options& options() const { return m_opt; }

private:
    struct Entry {
        uint32_t offset = 0;   // latest put record (uncached collections)
        uint16_t length = 0;
        uint32_t live_size = 0;// size of this document's record after cleanup
        PsramString doc;       // cached collections only
    };
    struct KeyHash {
        size_t operator()(const PsramString& s) const;
    };
    using Index = std::unordered_map<PsramString, Entry, KeyHash, std::equal_to<PsramString>,
                                     PsramAllocator<std::pair<const PsramString, Entry>>>;

    struct ScanResult;
    const CollectionDef* findCollection(uint8_t id) const;
    static PsramString indexKey(uint8_t collection, std::string_view key);

    bool openLocked();
    bool recoverTmp();
    bool writeHeader(LogFile& file, uint32_t generation);
    bool scan(LogFile& file, bool apply, ScanResult& result);
    void applyRecord(uint32_t offset, uint16_t length, uint8_t collection, uint8_t op,
                     std::string_view key, std::string_view value);
    bool writeRecord(uint8_t collection, uint8_t op, std::string_view key, std::string_view value);
    bool afterWrite();
    bool compactLocked();
    void maybeCompact();
    void postponeCompaction();
    bool setAside(const char* reason);

    Options m_opt;
    Io& m_io;
    std::string m_tmp_path;
    std::unique_ptr<LogFile> m_file;
    Index m_index;
    std::mutex m_mutex;

    uint32_t m_size = 0;
    uint32_t m_live = 0;
    uint32_t m_seq = 0;
    uint32_t m_generation = 0;
    uint32_t m_records = 0;
    uint32_t m_skipped = 0;
    uint32_t m_truncated = 0;
    uint32_t m_compactions = 0;
    bool m_unsynced = false;
    int64_t m_unsynced_since_ms = 0;
    bool m_compact_needed = false;
    uint32_t m_compact_retry_size = 0;
};

} // namespace nexus_db
