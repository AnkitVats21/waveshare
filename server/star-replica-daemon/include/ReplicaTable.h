#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <functional>
#include <cstring>
#include "core_sysdb/WalTypes.h"

namespace StarReplica {

/**
 * @brief Thread-safe in-memory replica of the Waveshare authoritative SysDb state.
 *
 * Indexed by (ComponentId, field_tag).
 * Updates are applied monotonically from incoming WAL_BATCH and SNAPSHOT records.
 */
class ReplicaTable {
public:
    using FieldKey = std::pair<uint8_t, uint8_t>; // (component_id, field_tag)
    using ChangeListener = std::function<void(uint8_t comp_id, uint8_t field_tag, const std::vector<uint8_t>& val)>;

    ReplicaTable() = default;

    void applyRecord(uint32_t seq, uint8_t comp_id, uint8_t field_tag, const uint8_t* val, size_t len) {
        std::vector<ChangeListener> listeners_copy;
        std::vector<uint8_t> val_copy;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (seq > m_head_seq) {
                m_head_seq = seq;
            }
            val_copy.assign(val, val + len);
            m_table[{comp_id, field_tag}] = val_copy;
            listeners_copy = m_listeners;
        }

        for (const auto& cb : listeners_copy) {
            cb(comp_id, field_tag, val_copy);
        }
    }

    uint32_t getHeadSeq() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_head_seq;
    }

    void setHeadSeq(uint32_t seq) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_head_seq = seq;
    }

    void addChangeListener(ChangeListener cb) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_listeners.push_back(std::move(cb));
    }

    // ── Strongly-typed accessor helpers ───────────────────────────────────────

    bool getRawField(uint8_t comp_id, uint8_t field_tag, std::vector<uint8_t>& out_val) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_table.find({comp_id, field_tag});
        if (it != m_table.end()) {
            out_val = it->second;
            return true;
        }
        return false;
    }

    bool getBool(uint8_t comp_id, uint8_t field_tag, bool default_val = false) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_table.find({comp_id, field_tag});
        if (it != m_table.end() && !it->second.empty()) {
            return it->second[0] != 0;
        }
        return default_val;
    }

    int32_t getInt32(uint8_t comp_id, uint8_t field_tag, int32_t default_val = 0) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_table.find({comp_id, field_tag});
        if (it != m_table.end() && it->second.size() >= sizeof(int32_t)) {
            int32_t v;
            std::memcpy(&v, it->second.data(), sizeof(v));
            return v;
        }
        return default_val;
    }

    uint32_t getUint32(uint8_t comp_id, uint8_t field_tag, uint32_t default_val = 0) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_table.find({comp_id, field_tag});
        if (it != m_table.end() && it->second.size() >= sizeof(uint32_t)) {
            uint32_t v;
            std::memcpy(&v, it->second.data(), sizeof(v));
            return v;
        }
        return default_val;
    }

    float getFloat(uint8_t comp_id, uint8_t field_tag, float default_val = 0.0f) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_table.find({comp_id, field_tag});
        if (it != m_table.end() && it->second.size() >= sizeof(float)) {
            float v;
            std::memcpy(&v, it->second.data(), sizeof(v));
            return v;
        }
        return default_val;
    }

    std::string getString(uint8_t comp_id, uint8_t field_tag, const std::string& default_val = "") const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_table.find({comp_id, field_tag});
        if (it != m_table.end() && !it->second.empty()) {
            const char* s = reinterpret_cast<const char*>(it->second.data());
            size_t max_len = it->second.size();
            size_t len = strnlen(s, max_len);
            return std::string(s, len);
        }
        return default_val;
    }

    size_t getFieldCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_table.size();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_table.clear();
        m_head_seq = 0;
    }

private:
    mutable std::mutex m_mutex;
    uint32_t m_head_seq = 0;
    std::map<FieldKey, std::vector<uint8_t>> m_table;
    std::vector<ChangeListener> m_listeners;
};

} // namespace StarReplica
