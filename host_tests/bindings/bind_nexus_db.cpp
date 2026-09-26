#include "bindings.h"

#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>

#include "TestdbDb.generated.h"
#include "nexus_db/Database.h"

using namespace nexus_db;

namespace {

// POSIX files with simulated power loss: once a budget runs out the "device"
// is dead and every later operation fails without touching the disk. The
// append that crosses the byte budget lands partially, like a cut-off write.
class FaultIo : public Io {
public:
    long write_budget = -1;  // bytes; -1 = unlimited
    long op_budget = -1;     // mutating operations; -1 = unlimited
    bool dead = false;
    bool busy = false;  // remove fails, like a file another handle holds open
    long ops = 0;
    long bytes = 0;
    std::string root;  // prefixed to every path, so device paths land in a temp dir

    std::string p(const char* path) const { return root + path; }

    bool step() {
        if (dead) return false;
        if (op_budget == 0) {
            dead = true;
            return false;
        }
        if (op_budget > 0) op_budget--;
        ops++;
        return true;
    }

    class File : public LogFile {
    public:
        File(FaultIo& io, int fd) : m_io(io), m_fd(fd) {}
        ~File() override { ::close(m_fd); }
        bool readAt(uint32_t offset, void* dst, size_t len) override {
            if (m_io.dead) return false;
            return pread(m_fd, dst, len, offset) == ssize_t(len);
        }
        bool append(const void* src, size_t len) override {
            if (!m_io.step()) return false;
            size_t n = len;
            if (m_io.write_budget >= 0 && long(len) > m_io.write_budget) {
                n = size_t(m_io.write_budget);
                m_io.dead = true;
            }
            off_t end = lseek(m_fd, 0, SEEK_END);
            if (n && pwrite(m_fd, src, n, end) != ssize_t(n)) return false;
            if (m_io.write_budget >= 0) m_io.write_budget -= long(n);
            m_io.bytes += long(n);
            return n == len;
        }
        long size() override {
            struct stat st;
            return fstat(m_fd, &st) == 0 ? long(st.st_size) : -1;
        }
        bool sync() override { return m_io.step(); }
        bool truncate(uint32_t length) override { return m_io.step() && ftruncate(m_fd, length) == 0; }

    private:
        FaultIo& m_io;
        int m_fd;
    };

    std::unique_ptr<LogFile> open(const char* path, bool create) override {
        if (dead) return nullptr;
        if (create && access(p(path).c_str(), F_OK) != 0 && !step()) return nullptr;
        int fd = ::open(p(path).c_str(), O_RDWR | (create ? O_CREAT : 0), 0644);
        return fd < 0 ? nullptr : std::make_unique<File>(*this, fd);
    }
    std::unique_ptr<LogFile> create(const char* path) override {
        if (!step()) return nullptr;
        int fd = ::open(p(path).c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        return fd < 0 ? nullptr : std::make_unique<File>(*this, fd);
    }
    bool exists(const char* path) override { return !dead && access(p(path).c_str(), F_OK) == 0; }
    bool remove(const char* path) override {
        if (busy && std::string(path).find(".tmp") == std::string::npos) return false;
        return step() && ::unlink(p(path).c_str()) == 0;
    }
    bool rename(const char* from, const char* to) override {
        if (!step()) return false;
        if (access(p(to).c_str(), F_OK) == 0) return false;  // like FATFS
        return ::rename(p(from).c_str(), p(to).c_str()) == 0;
    }
    bool mkdirs(const char* dir) override {
        if (dead) return false;
        std::string full = p(dir);
        for (size_t i = root.size() + 1; i <= full.size(); ++i) {
            if (i == full.size() || full[i] == '/') ::mkdir(full.substr(0, i).c_str(), 0755);
        }
        return true;
    }
};

// Owns the strings the Options point at.
class PyDb {
public:
    PyDb(const std::string& path, const std::string& name,
         const std::vector<std::tuple<int, std::string, bool>>& collections, bool every_commit,
         uint32_t compact_min_bytes, FaultIo& io)
        : m_path(path), m_name(name) {
        for (auto& [id, cname, cached] : collections) m_names.push_back(cname);
        for (size_t i = 0; i < collections.size(); ++i) {
            m_defs.push_back({uint8_t(std::get<0>(collections[i])), m_names[i].c_str(), std::get<2>(collections[i])});
        }
        Options o{};
        o.name = m_name.c_str();
        o.path = m_path.c_str();
        o.schema_hash = 0x1234;
        o.flush = every_commit ? Flush::EveryCommit : Flush::Batched;
        o.collections = m_defs.data();
        o.collection_count = m_defs.size();
        o.compact_min_bytes = compact_min_bytes;
        m_db = std::make_unique<Database>(o, io);
    }
    Database& db() { return *m_db; }

private:
    std::string m_path, m_name;
    std::vector<std::string> m_names;
    std::vector<CollectionDef> m_defs;
    std::unique_ptr<Database> m_db;
};

std::string_view sv(const nb::bytes& b) { return {b.c_str(), b.size()}; }

nb::dict trackToDict(const ndb::testdb::Track& t) {
    nb::dict d;
    d["title"] = t.title;
    d["duration_ms"] = t.duration_ms;
    d["play_count"] = t.play_count;
    d["volume"] = t.volume;
    d["gain"] = t.gain;
    d["color"] = nb::make_tuple(t.color.r, t.color.g, t.color.b);
    d["pinned"] = t.pinned;
    d["offset"] = t.offset;
    d["blob"] = nb::bytes(t.blob.data(), t.blob.size());
    d["label"] = t.label;
    return d;
}

}  // namespace

void init_nexus_db(nb::module_& m) {
    nb::module_ n = m.def_submodule("ndb");

    nb::class_<FaultIo>(n, "FaultIo")
        .def(nb::init<>())
        .def_rw("root", &FaultIo::root)
        .def_rw("write_budget", &FaultIo::write_budget)
        .def_rw("op_budget", &FaultIo::op_budget)
        .def_rw("dead", &FaultIo::dead)
        .def_rw("busy", &FaultIo::busy)
        .def_ro("ops", &FaultIo::ops)
        .def_ro("bytes", &FaultIo::bytes);

    nb::class_<PyDb>(n, "Database")
        .def(nb::init<const std::string&, const std::string&, const std::vector<std::tuple<int, std::string, bool>>&,
                      bool, uint32_t, FaultIo&>(),
             nb::arg("path"), nb::arg("name"), nb::arg("collections"), nb::arg("every_commit") = true,
             nb::arg("compact_min_bytes") = 64 * 1024, nb::arg("io"), nb::keep_alive<1, 7>())
        .def("open", [](PyDb& s) { return s.db().open(); })
        .def("close", [](PyDb& s) { s.db().close(); })
        .def("is_open", [](PyDb& s) { return s.db().isOpen(); })
        .def("put", [](PyDb& s, int c, const std::string& k, nb::bytes v) { return s.db().put(uint8_t(c), k, sv(v)); })
        .def("merge", [](PyDb& s, int c, const std::string& k, nb::bytes v) { return s.db().merge(uint8_t(c), k, sv(v)); })
        .def("remove", [](PyDb& s, int c, const std::string& k) { return s.db().remove(uint8_t(c), k); })
        .def("get", [](PyDb& s, int c, const std::string& k) -> std::optional<nb::bytes> {
            std::string out;
            if (!s.db().get(uint8_t(c), k, out)) return std::nullopt;
            return nb::bytes(out.data(), out.size());
        })
        .def("contains", [](PyDb& s, int c, const std::string& k) { return s.db().contains(uint8_t(c), k); })
        .def("count", [](PyDb& s, int c) { return s.db().count(uint8_t(c)); })
        .def("items", [](PyDb& s, int c) {
            nb::dict d;
            s.db().forEach(uint8_t(c), [&](std::string_view k, std::string_view v) {
                d[nb::str(k.data(), k.size())] = nb::bytes(v.data(), v.size());
                return true;
            });
            return d;
        })
        .def("flush", [](PyDb& s) { return s.db().flush(); })
        .def("compact", [](PyDb& s) { return s.db().compact(); })
        .def("stats", [](PyDb& s) {
            Stats st = s.db().stats();
            nb::dict d;
            d["file_bytes"] = st.file_bytes;
            d["live_bytes"] = st.live_bytes;
            d["records"] = st.records;
            d["skipped_bytes"] = st.skipped_bytes;
            d["truncated_bytes"] = st.truncated_bytes;
            d["generation"] = st.generation;
            d["compactions"] = st.compactions;
            d["documents"] = st.documents;
            return d;
        });

    // The generated typed API, on a real file.
    nb::class_<ndb::testdb::TestdbDb>(n, "TestdbDb")
        .def(nb::init<FaultIo&>(), nb::keep_alive<1, 2>())
        .def("open", &ndb::testdb::TestdbDb::open)
        .def("close", &ndb::testdb::TestdbDb::close)
        .def("put_track", [](ndb::testdb::TestdbDb& s, const std::string& key, const std::string& title,
                             uint32_t duration, uint32_t plays) {
            ndb::testdb::Track t;
            t.title = title;
            t.duration_ms = duration;
            t.play_count = plays;
            t.blob = std::string("\x00\x01\xff", 3);
            return s.tracks().put(key, t);
        })
        .def("merge_plays", [](ndb::testdb::TestdbDb& s, const std::string& key, uint32_t plays, bool pinned) {
            ndb::testdb::Track t;
            t.play_count = plays;
            t.pinned = pinned;
            return s.tracks().merge(key, t, ndb::testdb::Track::F_PLAY_COUNT | ndb::testdb::Track::F_PINNED);
        })
        .def("remove_track", [](ndb::testdb::TestdbDb& s, const std::string& key) { return s.tracks().remove(key); })
        .def("get_track", [](ndb::testdb::TestdbDb& s, const std::string& key) -> std::optional<nb::dict> {
            ndb::testdb::Track t;
            if (!s.tracks().get(key, t)) return std::nullopt;
            return trackToDict(t);
        })
        .def("default_track", [](ndb::testdb::TestdbDb&) { return trackToDict(ndb::testdb::Track{}); })
        .def("track_keys", [](ndb::testdb::TestdbDb& s) {
            std::vector<std::string> keys;
            s.tracks().forEach([&](std::string_view k, const ndb::testdb::Track&) {
                keys.emplace_back(k);
                return true;
            });
            return keys;
        })
        .def("put_blob", [](ndb::testdb::TestdbDb& s, const std::string& key, nb::bytes data, int32_t n) {
            ndb::testdb::Blob b;
            b.data.assign(data.c_str(), data.size());
            b.n = n;
            return s.blobs().put(key, b);
        })
        .def("get_blob", [](ndb::testdb::TestdbDb& s, const std::string& key) -> std::optional<nb::tuple> {
            ndb::testdb::Blob b;
            if (!s.blobs().get(key, b)) return std::nullopt;
            return nb::make_tuple(nb::bytes(b.data.data(), b.data.size()), b.n);
        })
        .def_prop_ro_static("schema_hash", [](nb::handle) { return ndb::testdb::SCHEMA_HASH; });
}
