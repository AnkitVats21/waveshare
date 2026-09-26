#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace nexus_db {

uint32_t crc32(const void* data, size_t len, uint32_t crc = 0);

struct Rgb {
    uint8_t r = 0, g = 0, b = 0;
    bool operator==(const Rgb& o) const { return r == o.r && g == o.g && b == o.b; }
    bool operator!=(const Rgb& o) const { return !(*this == o); }
};

// Appends fields (tag u16, length u16, value) to a document value.
// Integers are little-endian.
class Writer {
public:
    explicit Writer(std::string& out) : m_out(out) {}

    void raw(uint16_t tag, const void* data, size_t len) {
        if (len > 0xFFFF) len = 0xFFFF;
        uint8_t hdr[4] = {uint8_t(tag), uint8_t(tag >> 8), uint8_t(len), uint8_t(len >> 8)};
        m_out.append(reinterpret_cast<const char*>(hdr), 4);
        m_out.append(static_cast<const char*>(data), len);
    }
    void boolean(uint16_t tag, bool v) { u8(tag, v ? 1 : 0); }
    void u8(uint16_t tag, uint8_t v) { raw(tag, &v, 1); }
    void i8(uint16_t tag, int8_t v) { u8(tag, uint8_t(v)); }
    void u16(uint16_t tag, uint16_t v) { le(tag, v, 2); }
    void i16(uint16_t tag, int16_t v) { le(tag, uint16_t(v), 2); }
    void u32(uint16_t tag, uint32_t v) { le(tag, v, 4); }
    void i32(uint16_t tag, int32_t v) { le(tag, uint32_t(v), 4); }
    void u64(uint16_t tag, uint64_t v) { le(tag, v, 8); }
    void i64(uint16_t tag, int64_t v) { le(tag, uint64_t(v), 8); }
    void f32(uint16_t tag, float v) {
        uint32_t bits;
        memcpy(&bits, &v, 4);
        u32(tag, bits);
    }
    void str(uint16_t tag, std::string_view v) { raw(tag, v.data(), v.size()); }
    void bytes(uint16_t tag, std::string_view v) { raw(tag, v.data(), v.size()); }
    void rgb(uint16_t tag, Rgb v) {
        uint8_t b[3] = {v.r, v.g, v.b};
        raw(tag, b, 3);
    }

private:
    void le(uint16_t tag, uint64_t v, size_t n) {
        uint8_t b[8];
        for (size_t i = 0; i < n; ++i) b[i] = uint8_t(v >> (8 * i));
        raw(tag, b, n);
    }
    std::string& m_out;
};

// Walks the fields of a document value. Trailing bytes shorter than a field
// header are record padding and end the walk.
class FieldReader {
public:
    FieldReader(const uint8_t* data, size_t len) : m_p(data), m_end(data + len) {}

    // False at the end, or if a field runs past the value (see ok()).
    bool next(uint16_t& tag, const uint8_t*& value, uint16_t& len) {
        if (m_end - m_p < 4) return false;
        tag = uint16_t(m_p[0] | (m_p[1] << 8));
        len = uint16_t(m_p[2] | (m_p[3] << 8));
        if (size_t(m_end - m_p - 4) < len) {
            m_bad = true;
            return false;
        }
        value = m_p + 4;
        m_p += 4 + len;
        return true;
    }
    bool ok() const { return !m_bad; }
    // Where the walk stopped: the end of the last whole field.
    const uint8_t* position() const { return m_p; }

    static uint64_t le(const uint8_t* p, size_t n) {
        uint64_t v = 0;
        for (size_t i = 0; i < n; ++i) v |= uint64_t(p[i]) << (8 * i);
        return v;
    }
    // Typed reads: false (and `out` untouched) if the length doesn't match
    // the type, so a field that changed type falls back to its default.
    static bool read(const uint8_t* p, uint16_t n, bool& out) { return n == 1 ? (out = p[0] != 0, true) : false; }
    static bool read(const uint8_t* p, uint16_t n, uint8_t& out) { return n == 1 ? (out = p[0], true) : false; }
    static bool read(const uint8_t* p, uint16_t n, int8_t& out) { return n == 1 ? (out = int8_t(p[0]), true) : false; }
    static bool read(const uint8_t* p, uint16_t n, uint16_t& out) { return n == 2 ? (out = uint16_t(le(p, 2)), true) : false; }
    static bool read(const uint8_t* p, uint16_t n, int16_t& out) { return n == 2 ? (out = int16_t(le(p, 2)), true) : false; }
    static bool read(const uint8_t* p, uint16_t n, uint32_t& out) { return n == 4 ? (out = uint32_t(le(p, 4)), true) : false; }
    static bool read(const uint8_t* p, uint16_t n, int32_t& out) { return n == 4 ? (out = int32_t(le(p, 4)), true) : false; }
    static bool read(const uint8_t* p, uint16_t n, uint64_t& out) { return n == 8 ? (out = le(p, 8), true) : false; }
    static bool read(const uint8_t* p, uint16_t n, int64_t& out) { return n == 8 ? (out = int64_t(le(p, 8)), true) : false; }
    static bool read(const uint8_t* p, uint16_t n, float& out) {
        if (n != 4) return false;
        uint32_t bits = uint32_t(le(p, 4));
        memcpy(&out, &bits, 4);
        return true;
    }
    static bool read(const uint8_t* p, uint16_t n, std::string& out) {
        out.assign(reinterpret_cast<const char*>(p), n);
        return true;
    }
    static bool read(const uint8_t* p, uint16_t n, Rgb& out) {
        if (n != 3) return false;
        out = {p[0], p[1], p[2]};
        return true;
    }

private:
    const uint8_t* m_p;
    const uint8_t* m_end;
    bool m_bad = false;
};

} // namespace nexus_db
