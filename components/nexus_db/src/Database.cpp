#include "nexus_db/Database.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "esp_log.h"
#include "nexus_db/Fields.h"

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#else
#include <chrono>
#endif

namespace nexus_db {
namespace {

const char* const TAG = "nexus_db";

constexpr uint32_t HEADER_SIZE = 32;
constexpr uint32_t RECORD_HEADER = 16;
constexpr uint16_t SYNC_MARKER = 0x5AA5;
constexpr uint32_t MAX_RECORD = 0xFFFC;  // 16-bit length, 4-byte aligned
constexpr uint32_t SCAN_WINDOW = 64 * 1024;
constexpr uint32_t COMPACT_BUFFER = 16 * 1024;
constexpr uint32_t SEAL_RECORD_SIZE = RECORD_HEADER + 4 + 4;  // one u32 field

enum Op : uint8_t { OP_PUT = 1, OP_MERGE = 2, OP_DELETE = 3, OP_SEAL = 4 };

enum class HeaderStatus { Ok, Bad, Newer };

int64_t nowMs() {
#ifdef ESP_PLATFORM
    return esp_timer_get_time() / 1000;
#else
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
#endif
}

uint32_t align4(uint32_t n) { return (n + 3) & ~3u; }
uint32_t recordSize(size_t key_len, size_t value_len) {
    return align4(uint32_t(RECORD_HEADER + key_len + value_len));
}

void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
uint16_t get16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t get32(const uint8_t* p) { return uint32_t(FieldReader::le(p, 4)); }

// Appends one encoded record to `out`.
void encodeRecord(std::string& out, uint32_t seq, uint8_t collection, uint8_t op,
                  std::string_view key, std::string_view value) {
    uint32_t len = recordSize(key.size(), value.size());
    size_t start = out.size();
    out.resize(start + len, '\0');
    uint8_t* r = reinterpret_cast<uint8_t*>(&out[start]);
    put16(r, SYNC_MARKER);
    put16(r + 2, uint16_t(len));
    put32(r + 8, seq);
    r[12] = collection;
    r[13] = op;
    r[14] = uint8_t(key.size());
    r[15] = 0;
    memcpy(r + RECORD_HEADER, key.data(), key.size());
    memcpy(r + RECORD_HEADER + key.size(), value.data(), value.size());
    put32(r + 4, crc32(r + 8, len - 8));
}

struct Record {
    uint16_t length;
    uint32_t seq;
    uint8_t collection;
    uint8_t op;
    std::string_view key;
    std::string_view value;  // fields only, padding stripped
};

// Validates the record at the start of `p` (`avail` bytes readable).
bool parseRecord(const uint8_t* p, size_t avail, Record& rec) {
    if (avail < RECORD_HEADER || get16(p) != SYNC_MARKER) return false;
    uint16_t len = get16(p + 2);
    if (len < RECORD_HEADER || (len & 3) || len > avail) return false;
    if (crc32(p + 8, len - 8) != get32(p + 4)) return false;
    uint8_t op = p[13];
    uint8_t key_len = p[14];
    if (op < OP_PUT || op > OP_SEAL || key_len > Database::MAX_KEY || RECORD_HEADER + key_len > len) {
        return false;
    }
    const uint8_t* value = p + RECORD_HEADER + key_len;
    FieldReader fields(value, len - RECORD_HEADER - key_len);
    uint16_t tag, flen;
    const uint8_t* fv;
    while (fields.next(tag, fv, flen)) {
    }
    if (!fields.ok()) return false;
    rec.length = len;
    rec.seq = get32(p + 8);
    rec.collection = p[12];
    rec.op = op;
    rec.key = {reinterpret_cast<const char*>(p + RECORD_HEADER), key_len};
    rec.value = {reinterpret_cast<const char*>(value), size_t(fields.position() - value)};
    return true;
}

// Reads a file through a window large enough for the biggest record.
class ScanReader {
public:
    ScanReader(LogFile& file, uint32_t size) : m_file(file), m_size(size) {
        m_buf = PsramAllocator<uint8_t>().allocate(SCAN_WINDOW);
    }
    ~ScanReader() { PsramAllocator<uint8_t>().deallocate(m_buf, SCAN_WINDOW); }

    // Pointer to the bytes at `offset`; `avail` is how many are readable
    // (up to a whole maximum-size record, or to the end of the file).
    const uint8_t* at(uint32_t offset, size_t& avail) {
        uint32_t want = std::min<uint32_t>(MAX_RECORD, m_size - offset);
        if (offset < m_start || offset + want > m_start + m_len) {
            m_start = offset;
            m_len = std::min<uint32_t>(SCAN_WINDOW, m_size - offset);
            if (!m_file.readAt(offset, m_buf, m_len)) {
                m_len = 0;
                avail = 0;
                m_error = true;
                return nullptr;
            }
        }
        avail = m_start + m_len - offset;
        return m_buf + (offset - m_start);
    }
    bool error() const { return m_error; }

private:
    LogFile& m_file;
    uint32_t m_size;
    uint8_t* m_buf;
    uint32_t m_start = 0;
    uint32_t m_len = 0;
    bool m_error = false;
};

HeaderStatus checkHeader(const uint8_t* h, const char* name) {
    if (memcmp(h, "NXDB", 4) != 0) return HeaderStatus::Bad;
    if (crc32(h, 28) != get32(h + 28)) return HeaderStatus::Bad;
    if (get16(h + 4) > Database::FORMAT_VERSION) return HeaderStatus::Newer;
    if (get16(h + 6) != HEADER_SIZE) return HeaderStatus::Bad;
    char stored[13] = {};
    memcpy(stored, h + 16, 12);
    return strcmp(stored, name) == 0 ? HeaderStatus::Ok : HeaderStatus::Bad;
}

// Replaces the fields of `doc` that appear in `patch`.
void mergeFields(PsramString& doc, std::string_view patch) {
    const auto* pp = reinterpret_cast<const uint8_t*>(patch.data());
    auto inPatch = [&](uint16_t tag) {
        FieldReader r(pp, patch.size());
        uint16_t t, l;
        const uint8_t* v;
        while (r.next(t, v, l)) {
            if (t == tag) return true;
        }
        return false;
    };
    PsramString out;
    out.reserve(doc.size() + patch.size());
    FieldReader r(reinterpret_cast<const uint8_t*>(doc.data()), doc.size());
    uint16_t tag, len;
    const uint8_t* v;
    while (r.next(tag, v, len)) {
        if (!inPatch(tag)) out.append(reinterpret_cast<const char*>(v - 4), len + 4);
    }
    out.append(patch.data(), patch.size());
    doc.swap(out);
}

}  // namespace

struct Database::ScanResult {
    uint32_t end = HEADER_SIZE;  // end of the last valid record
    uint32_t records = 0;
    uint32_t skipped = 0;
    uint32_t truncated = 0;
    uint32_t max_seq = 0;
    uint8_t last_op = 0;
    uint32_t seal_count = 0;
    bool read_error = false;
};

size_t Database::KeyHash::operator()(const PsramString& s) const {
    uint32_t h = 2166136261u;  // FNV-1a
    for (char c : s) h = (h ^ uint8_t(c)) * 16777619u;
    return h;
}

Database::Database(const Options& options, Io& io)
    : m_opt(options), m_io(io), m_tmp_path(std::string(options.path) + ".tmp") {}

Database::~Database() { close(); }

const CollectionDef* Database::findCollection(uint8_t id) const {
    for (size_t i = 0; i < m_opt.collection_count; ++i) {
        if (m_opt.collections[i].id == id) return &m_opt.collections[i];
    }
    return nullptr;
}

PsramString Database::indexKey(uint8_t collection, std::string_view key) {
    PsramString k;
    k.reserve(key.size() + 1);
    k.push_back(char(collection));
    k.append(key.data(), key.size());
    return k;
}

bool Database::isOpen() const { return m_file != nullptr; }

bool Database::open() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return openLocked();
}

void Database::close() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file && m_unsynced) m_file->sync();
    m_file.reset();
    m_unsynced = false;
    m_index.clear();
}

bool Database::writeHeader(LogFile& file, uint32_t generation) {
    uint8_t h[HEADER_SIZE] = {};
    memcpy(h, "NXDB", 4);
    put16(h + 4, FORMAT_VERSION);
    put16(h + 6, HEADER_SIZE);
    put32(h + 8, m_opt.schema_hash);
    put32(h + 12, generation);
    strncpy(reinterpret_cast<char*>(h + 16), m_opt.name, 11);
    put32(h + 28, crc32(h, 28));
    return file.append(h, sizeof(h));
}

bool Database::scan(LogFile& file, bool apply, ScanResult& res) {
    long file_size = file.size();
    if (file_size < long(HEADER_SIZE)) return false;
    uint32_t size = uint32_t(file_size);
    ScanReader reader(file, size);
    Record rec;
    auto recordAt = [&](uint32_t pos) {
        size_t avail;
        const uint8_t* p = reader.at(pos, avail);
        return p && parseRecord(p, avail, rec);
    };

    uint32_t pos = HEADER_SIZE;
    while (pos < size) {
        if (recordAt(pos)) {
            if (apply) applyRecord(pos, rec.length, rec.collection, rec.op, rec.key, rec.value);
            res.records++;
            res.max_seq = std::max(res.max_seq, rec.seq);
            res.last_op = rec.op;
            if (rec.op == OP_SEAL) {
                uint16_t tag, len;
                const uint8_t* v;
                FieldReader fr(reinterpret_cast<const uint8_t*>(rec.value.data()), rec.value.size());
                res.seal_count = (fr.next(tag, v, len) && tag == 1 && len == 4) ? get32(v) : 0;
            }
            pos += rec.length;
            continue;
        }
        if (reader.error()) {
            res.read_error = true;
            break;
        }
        // Look for the next valid record. None means this is a write cut off
        // by a reset; one means the bytes in between are damaged.
        uint32_t next = pos + 4;
        while (next + RECORD_HEADER <= size && !recordAt(next)) {
            if (reader.error()) break;
            next += 4;
        }
        if (reader.error()) {
            res.read_error = true;
            break;
        }
        if (next + RECORD_HEADER > size) {
            res.truncated = size - pos;
            break;
        }
        ESP_LOGW(TAG, "%s: skipped %u damaged bytes at offset %u", m_opt.name, unsigned(next - pos),
                 unsigned(pos));
        res.skipped += next - pos;
        pos = next;
    }
    res.end = pos;
    return !res.read_error;
}

void Database::applyRecord(uint32_t offset, uint16_t length, uint8_t collection, uint8_t op,
                           std::string_view key, std::string_view value) {
    if (op == OP_SEAL) return;
    const CollectionDef* def = findCollection(collection);
    if (!def) return;  // a collection this firmware doesn't know; dropped at cleanup

    PsramString k = indexKey(collection, key);
    auto it = m_index.find(k);
    if (op == OP_DELETE) {
        if (it != m_index.end()) {
            m_live -= it->second.live_size;
            m_index.erase(it);
        }
        return;
    }
    if (op == OP_MERGE && !def->cached) return;  // never written; ignore

    if (it == m_index.end()) it = m_index.emplace(std::move(k), Entry{}).first;
    Entry& e = it->second;
    m_live -= e.live_size;
    if (op == OP_PUT) {
        e.offset = offset;
        e.length = length;
        if (def->cached) e.doc.assign(value.data(), value.size());
        e.live_size = recordSize(key.size(), value.size());
    } else {
        mergeFields(e.doc, value);
        e.live_size = recordSize(key.size(), e.doc.size());
    }
    m_live += e.live_size;
}

bool Database::setAside(const char* reason) {
    std::string bad = std::string(m_opt.path) + ".bad";
    ESP_LOGE(TAG, "%s: %s; moving it to %s and starting empty", m_opt.name, reason, bad.c_str());
    m_file.reset();
    if (m_io.exists(bad.c_str())) m_io.remove(bad.c_str());
    if (!m_io.rename(m_opt.path, bad.c_str())) return false;
    m_file = m_io.create(m_opt.path);
    return m_file && writeHeader(*m_file, 0) && m_file->sync();
}

bool Database::recoverTmp() {
    const char* tmp = m_tmp_path.c_str();
    if (!m_io.exists(tmp)) return true;
    if (m_io.exists(m_opt.path)) {
        // Cleanup stopped before the swap; the original is intact.
        ESP_LOGW(TAG, "%s: discarding an interrupted cleanup", m_opt.name);
        return m_io.remove(tmp);
    }
    // Cleanup stopped between removing the original and renaming the copy.
    // The copy is complete only if its seal record is there and matches.
    bool sealed = false;
    {
        std::unique_ptr<LogFile> f = m_io.open(tmp, false);
        uint8_t h[HEADER_SIZE];
        ScanResult res;
        if (f && f->readAt(0, h, sizeof(h)) && checkHeader(h, m_opt.name) == HeaderStatus::Ok &&
            scan(*f, false, res)) {
            sealed = res.last_op == OP_SEAL && res.skipped == 0 && res.truncated == 0 &&
                     res.seal_count + 1 == res.records;
        }
    }
    if (!sealed) {
        ESP_LOGE(TAG, "%s: %s has no valid seal; deleting it", m_opt.name, tmp);
        return m_io.remove(tmp);
    }
    ESP_LOGW(TAG, "%s: finishing an interrupted cleanup", m_opt.name);
    return m_io.rename(tmp, m_opt.path);
}

bool Database::openLocked() {
    m_file.reset();
    m_index.clear();
    m_live = HEADER_SIZE + SEAL_RECORD_SIZE;
    m_size = 0;
    m_seq = 0;
    m_records = m_skipped = m_truncated = 0;
    m_unsynced = false;
    m_compact_needed = false;

    std::string dir(m_opt.path);
    size_t slash = dir.rfind('/');
    if (slash != std::string::npos && slash > 0) {
        dir.resize(slash);
        m_io.mkdirs(dir.c_str());
    }
    if (!recoverTmp()) ESP_LOGW(TAG, "%s: could not resolve %s", m_opt.name, m_tmp_path.c_str());

    m_file = m_io.open(m_opt.path, true);
    if (!m_file) {
        ESP_LOGE(TAG, "%s: cannot open %s", m_opt.name, m_opt.path);
        return false;
    }
    long size = m_file->size();
    if (size < long(HEADER_SIZE)) {
        // New file, or one whose header write was cut off.
        if (size > 0 && !m_file->truncate(0)) return false;
        m_generation = 0;
        if (!writeHeader(*m_file, 0) || !m_file->sync()) {
            m_file.reset();
            return false;
        }
        m_size = HEADER_SIZE;
        return true;
    }

    uint8_t h[HEADER_SIZE];
    HeaderStatus status = m_file->readAt(0, h, sizeof(h)) ? checkHeader(h, m_opt.name) : HeaderStatus::Bad;
    if (status == HeaderStatus::Newer) {
        ESP_LOGE(TAG, "%s: written by newer firmware (format %u); not opened", m_opt.name, get16(h + 4));
        m_file.reset();
        return false;
    }
    if (status == HeaderStatus::Bad) {
        if (!setAside("bad header")) {
            m_file.reset();
            return false;
        }
        m_generation = 0;
        m_size = HEADER_SIZE;
        return true;
    }
    m_generation = get32(h + 12);

    ScanResult res;
    if (!scan(*m_file, true, res)) {
        ESP_LOGE(TAG, "%s: read error while opening", m_opt.name);
        m_file.reset();
        m_index.clear();
        return false;
    }
    if (res.truncated) {
        ESP_LOGW(TAG, "%s: removed a cut-off write of %u bytes at the end", m_opt.name, unsigned(res.truncated));
        if (!m_file->truncate(res.end) || !m_file->sync()) {
            m_file.reset();
            return false;
        }
    }
    m_size = res.end;
    m_seq = res.max_seq;
    m_records = res.records;
    m_skipped = res.skipped;
    m_truncated = res.truncated;
    // Damage is dropped by a cleanup, and so is a stale schema hash: the
    // header is rewritten only then, and readers compare it with theirs.
    m_compact_needed = res.skipped > 0 || get32(h + 8) != m_opt.schema_hash;
    ESP_LOGI(TAG, "%s: %u documents, %u records, %u bytes (%u live)", m_opt.name, unsigned(m_index.size()),
             unsigned(m_records), unsigned(m_size), unsigned(m_live));
    maybeCompact();
    return m_file != nullptr;
}

bool Database::writeRecord(uint8_t collection, uint8_t op, std::string_view key, std::string_view value) {
    if (!m_file || key.size() > MAX_KEY || recordSize(key.size(), value.size()) > MAX_RECORD) return false;
    std::string buf;
    encodeRecord(buf, m_seq + 1, collection, op, key, value);
    if (!m_file->append(buf.data(), buf.size())) {
        // Don't leave a partial record for later records to follow.
        m_file->truncate(m_size);
        ESP_LOGE(TAG, "%s: write failed", m_opt.name);
        return false;
    }
    uint32_t offset = m_size;
    m_seq++;
    m_size += buf.size();
    m_records++;
    applyRecord(offset, uint16_t(buf.size()), collection, op, key, value);
    return afterWrite();
}

bool Database::afterWrite() {
    bool ok = true;
    if (m_opt.flush == Flush::EveryCommit) {
        ok = m_file->sync();
    } else {
        int64_t now = nowMs();
        if (!m_unsynced) {
            m_unsynced = true;
            m_unsynced_since_ms = now;
        }
        if (now - m_unsynced_since_ms >= FLUSH_INTERVAL_MS) {
            ok = m_file->sync();
            m_unsynced = false;
        }
    }
    maybeCompact();
    return ok;
}

bool Database::put(uint8_t collection, std::string_view key, std::string_view value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const CollectionDef* def = findCollection(collection);
    if (!def || !m_file) return false;
    if (def->cached) {
        auto it = m_index.find(indexKey(collection, key));
        if (it != m_index.end() && std::string_view(it->second.doc.data(), it->second.doc.size()) == value) {
            return true;  // unchanged
        }
    }
    return writeRecord(collection, OP_PUT, key, value);
}

bool Database::merge(uint8_t collection, std::string_view key, std::string_view value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const CollectionDef* def = findCollection(collection);
    if (!def || !def->cached || !m_file) return false;
    auto it = m_index.find(indexKey(collection, key));
    if (it != m_index.end()) {
        PsramString merged = it->second.doc;
        mergeFields(merged, value);
        if (merged == it->second.doc) return true;  // unchanged
    }
    return writeRecord(collection, OP_MERGE, key, value);
}

bool Database::remove(uint8_t collection, std::string_view key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!findCollection(collection) || !m_file) return false;
    if (m_index.find(indexKey(collection, key)) == m_index.end()) return true;
    return writeRecord(collection, OP_DELETE, key, {});
}

bool Database::get(uint8_t collection, std::string_view key, std::string& out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const CollectionDef* def = findCollection(collection);
    if (!def || !m_file) return false;
    auto it = m_index.find(indexKey(collection, key));
    if (it == m_index.end()) return false;
    const Entry& e = it->second;
    if (def->cached) {
        out.assign(e.doc.data(), e.doc.size());
        return true;
    }
    std::string buf(e.length, '\0');
    Record rec;
    if (!m_file->readAt(e.offset, &buf[0], e.length) ||
        !parseRecord(reinterpret_cast<const uint8_t*>(buf.data()), buf.size(), rec)) {
        ESP_LOGE(TAG, "%s: record at %u no longer reads back", m_opt.name, unsigned(e.offset));
        return false;
    }
    out.assign(rec.value.data(), rec.value.size());
    return true;
}

bool Database::contains(uint8_t collection, std::string_view key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_index.find(indexKey(collection, key)) != m_index.end();
}

size_t Database::count(uint8_t collection) {
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t n = 0;
    for (const auto& [k, e] : m_index) n += uint8_t(k[0]) == collection;
    return n;
}

void Database::forEach(uint8_t collection, const Visitor& fn) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const CollectionDef* def = findCollection(collection);
    if (!def || !m_file) return;
    std::string buf;
    for (const auto& [k, e] : m_index) {
        if (uint8_t(k[0]) != collection) continue;
        std::string_view key(k.data() + 1, k.size() - 1);
        if (def->cached) {
            if (!fn(key, std::string_view(e.doc.data(), e.doc.size()))) return;
            continue;
        }
        buf.resize(e.length);
        Record rec;
        if (m_file->readAt(e.offset, &buf[0], e.length) &&
            parseRecord(reinterpret_cast<const uint8_t*>(buf.data()), buf.size(), rec)) {
            if (!fn(key, rec.value)) return;
        }
    }
}

bool Database::flush() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_file || !m_unsynced) return true;
    m_unsynced = false;
    return m_file->sync();
}

bool Database::flushIfDue() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_file || !m_unsynced || nowMs() - m_unsynced_since_ms < FLUSH_INTERVAL_MS) return true;
    m_unsynced = false;
    return m_file->sync();
}

bool Database::compact() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return compactLocked();
}

void Database::maybeCompact() {
    if (!m_file) return;
    bool mostly_dead = m_size > m_opt.compact_min_bytes && m_size >= m_compact_retry_size &&
                       m_size - std::min(m_size, m_live) > m_live;
    if (m_compact_needed || mostly_dead) compactLocked();
}

void Database::postponeCompaction() {
    // Retry after the file grows a bit, not on every write.
    m_compact_needed = false;
    m_compact_retry_size = m_size + 16 * 1024;
}

bool Database::compactLocked() {
    if (!m_file) return false;
    int64_t started = nowMs();
    const char* tmp = m_tmp_path.c_str();
    uint32_t new_gen = m_generation + 1;

    // 1. Write every live document to <path>.tmp, then a seal record.
    std::vector<std::pair<Entry*, uint32_t>, PsramAllocator<std::pair<Entry*, uint32_t>>> moved;
    moved.reserve(m_index.size());
    uint32_t written = HEADER_SIZE;
    uint32_t seq = 0;
    bool ok;
    {
        std::unique_ptr<LogFile> out = m_io.create(tmp);
        ok = out && writeHeader(*out, new_gen);
        std::string buf;
        std::string value;
        buf.reserve(COMPACT_BUFFER + MAX_RECORD);
        for (auto it = m_index.begin(); ok && it != m_index.end(); ++it) {
            uint8_t collection = uint8_t(it->first[0]);
            std::string_view key(it->first.data() + 1, it->first.size() - 1);
            Entry& e = it->second;
            std::string_view v;
            if (findCollection(collection)->cached) {
                v = std::string_view(e.doc.data(), e.doc.size());
            } else {
                value.resize(e.length);
                Record rec;
                if (!m_file->readAt(e.offset, &value[0], e.length) ||
                    !parseRecord(reinterpret_cast<const uint8_t*>(value.data()), value.size(), rec)) {
                    ESP_LOGE(TAG, "%s: cleanup could not read the record at %u", m_opt.name, unsigned(e.offset));
                    ok = false;
                    break;
                }
                v = rec.value;
            }
            size_t before = buf.size();
            encodeRecord(buf, ++seq, collection, OP_PUT, key, v);
            moved.emplace_back(&e, written);
            written += buf.size() - before;
            if (buf.size() >= COMPACT_BUFFER) {
                ok = out->append(buf.data(), buf.size());
                buf.clear();
            }
        }
        if (ok) {
            std::string seal;
            Writer(seal).u32(1, seq);
            encodeRecord(buf, seq + 1, 0, OP_SEAL, {}, seal);
            written += SEAL_RECORD_SIZE;
            ok = out->append(buf.data(), buf.size()) && out->sync();
        }
    }
    if (!ok) {
        ESP_LOGE(TAG, "%s: cleanup failed while writing %s", m_opt.name, tmp);
        m_io.remove(tmp);
        postponeCompaction();
        return false;
    }

    // 2. Swap it in. FATFS can't rename over an existing file, so the old one
    //    goes first; a reset in between is finished by recoverTmp() at open.
    if (m_unsynced) m_file->sync();
    m_file.reset();
    if (!m_io.remove(m_opt.path)) {
        // Usually the file is open elsewhere (a dashboard download).
        ESP_LOGW(TAG, "%s: cleanup postponed; file is busy", m_opt.name);
        m_io.remove(tmp);
        m_file = m_io.open(m_opt.path, false);
        postponeCompaction();
        return false;
    }
    if (!m_io.rename(tmp, m_opt.path)) {
        ESP_LOGE(TAG, "%s: rename after cleanup failed; reopening", m_opt.name);
        return openLocked();
    }
    m_file = m_io.open(m_opt.path, false);
    if (!m_file) return false;

    for (auto& [e, offset] : moved) {
        e->offset = offset;
        e->length = uint16_t(e->live_size);
    }
    ESP_LOGI(TAG, "%s: cleanup %u -> %u bytes in %d ms", m_opt.name, unsigned(m_size), unsigned(written),
             int(nowMs() - started));
    m_size = written;
    m_seq = seq + 1;
    m_records = seq + 1;
    m_generation = new_gen;
    m_compactions++;
    m_compact_retry_size = 0;
    m_unsynced = false;
    m_compact_needed = false;
    return true;
}

Stats Database::stats() {
    std::lock_guard<std::mutex> lock(m_mutex);
    Stats s;
    s.file_bytes = m_size;
    s.live_bytes = m_live;
    s.records = m_records;
    s.skipped_bytes = m_skipped;
    s.truncated_bytes = m_truncated;
    s.generation = m_generation;
    s.compactions = m_compactions;
    s.documents = uint32_t(m_index.size());
    return s;
}

}  // namespace nexus_db
