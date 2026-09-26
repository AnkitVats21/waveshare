#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "nexus_db/Database.h"
#include "nexus_db/Fields.h"

namespace nexus_db {

// Typed view of one collection. `Doc` is a starc-generated struct with
// COLLECTION, F_ALL, encode(Writer&, fields) and decode(value).
template <typename Doc>
class Collection {
public:
    explicit Collection(Database& db) : m_db(db) {}

    // `out` is reset to defaults first; fields missing from the file keep them.
    bool get(std::string_view key, Doc& out) {
        std::string value;
        if (!m_db.get(Doc::COLLECTION, key, value)) return false;
        out = Doc{};
        out.decode(value);
        return true;
    }
    bool put(std::string_view key, const Doc& doc) {
        std::string value;
        Writer w(value);
        doc.encode(w, Doc::F_ALL);
        return m_db.put(Doc::COLLECTION, key, value);
    }
    // Writes only the fields in `fields` (Doc::F_* bits).
    bool merge(std::string_view key, const Doc& doc, uint64_t fields) {
        std::string value;
        Writer w(value);
        doc.encode(w, fields);
        return m_db.merge(Doc::COLLECTION, key, value);
    }
    bool remove(std::string_view key) { return m_db.remove(Doc::COLLECTION, key); }
    bool contains(std::string_view key) { return m_db.contains(Doc::COLLECTION, key); }
    size_t count() { return m_db.count(Doc::COLLECTION); }

    // fn(std::string_view key, const Doc& doc) -> bool (false stops). Runs with
    // the database locked; don't call back into it.
    template <typename Fn>
    void forEach(Fn&& fn) {
        m_db.forEach(Doc::COLLECTION, [&](std::string_view key, std::string_view value) {
            Doc doc;
            doc.decode(value);
            return fn(key, static_cast<const Doc&>(doc));
        });
    }

private:
    Database& m_db;
};

} // namespace nexus_db
