#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include "core_sysdb/WalTypes.h"

namespace StarProtocol {

enum class MsgType : uint8_t {
    WAL_BATCH       = 0x01, ///< Host -> Client: Array of WalWireRecord
    REQ_CATCHUP     = 0x02, ///< Client -> Host: [since_seq: 4B]
    SNAPSHOT_START  = 0x03, ///< Host -> Client: [head_seq: 4B][total_records: 2B]
    SNAPSHOT_FIELD  = 0x04, ///< Host -> Client: [comp_id: 1B][field_tag: 1B][val_len: 1B][value: val_len]
    SNAPSHOT_END    = 0x05, ///< Host -> Client: [head_seq: 4B]
    CMD_SET_FIELD   = 0x06, ///< Client -> Host: [comp_id: 1B][field_tag: 1B][val_len: 1B][value: val_len]
    CMD_EXEC_ACTION = 0x07, ///< Client -> Host: [cmd_id: 1B][nonce: 4B][param: 4B][data_len: 1B][data: data_len]
    CMD_ACK         = 0x08  ///< Host -> Client: [status: 1B][seq: 4B]
};

struct __attribute__((packed)) FrameHeader {
    uint8_t  msg_type;
    uint16_t length; ///< Network byte order (big-endian)
};

static constexpr size_t HEADER_SIZE = sizeof(FrameHeader); // 3 bytes

inline uint16_t readU16BE(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline void writeU16BE(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}

inline uint32_t readU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
          (static_cast<uint32_t>(p[1]) << 8) |
          (static_cast<uint32_t>(p[2]) << 16) |
          (static_cast<uint32_t>(p[3]) << 24);
}

inline void writeU32LE(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

// ── Serialization Helpers ─────────────────────────────────────────────────────

inline std::vector<uint8_t> buildAckFrame(WriteResult status, uint32_t seq) {
    std::vector<uint8_t> frame(HEADER_SIZE + 5);
    frame[0] = static_cast<uint8_t>(MsgType::CMD_ACK);
    writeU16BE(&frame[1], 5);
    frame[3] = static_cast<uint8_t>(status);
    writeU32LE(&frame[4], seq);
    return frame;
}

inline std::vector<uint8_t> buildSnapshotStartFrame(uint32_t head_seq, uint16_t total_records) {
    std::vector<uint8_t> frame(HEADER_SIZE + 6);
    frame[0] = static_cast<uint8_t>(MsgType::SNAPSHOT_START);
    writeU16BE(&frame[1], 6);
    writeU32LE(&frame[3], head_seq);
    writeU16BE(&frame[7], total_records);
    return frame;
}

inline std::vector<uint8_t> buildSnapshotFieldFrame(uint8_t comp_id, uint8_t field_tag, const uint8_t* val, uint8_t val_len) {
    std::vector<uint8_t> frame(HEADER_SIZE + 3 + val_len);
    frame[0] = static_cast<uint8_t>(MsgType::SNAPSHOT_FIELD);
    writeU16BE(&frame[1], static_cast<uint16_t>(3 + val_len));
    frame[3] = comp_id;
    frame[4] = field_tag;
    frame[5] = val_len;
    if (val_len > 0 && val != nullptr) {
        std::memcpy(&frame[6], val, val_len);
    }
    return frame;
}

inline std::vector<uint8_t> buildSnapshotEndFrame(uint32_t head_seq) {
    std::vector<uint8_t> frame(HEADER_SIZE + 4);
    frame[0] = static_cast<uint8_t>(MsgType::SNAPSHOT_END);
    writeU16BE(&frame[1], 4);
    writeU32LE(&frame[3], head_seq);
    return frame;
}

inline std::vector<uint8_t> buildWalBatchFrame(const std::vector<WalRecordEntry>& records) {
    size_t payload_len = 0;
    for (const auto& r : records) {
        payload_len += sizeof(WalWireRecord) + r.value.size();
    }
    std::vector<uint8_t> frame(HEADER_SIZE + payload_len);
    frame[0] = static_cast<uint8_t>(MsgType::WAL_BATCH);
    writeU16BE(&frame[1], static_cast<uint16_t>(payload_len));

    size_t offset = HEADER_SIZE;
    for (const auto& r : records) {
        auto* wire = reinterpret_cast<WalWireRecord*>(&frame[offset]);
        wire->seq = r.seq;
        wire->component_id = r.component_id;
        wire->field_tag = r.field_tag;
        wire->val_len = static_cast<uint8_t>(r.value.size());
        if (!r.value.empty()) {
            std::memcpy(wire->value, r.value.data(), r.value.size());
        }
        offset += sizeof(WalWireRecord) + r.value.size();
    }
    return frame;
}

inline std::vector<uint8_t> buildCatchupReqFrame(uint32_t since_seq) {
    std::vector<uint8_t> frame(HEADER_SIZE + 4);
    frame[0] = static_cast<uint8_t>(MsgType::REQ_CATCHUP);
    writeU16BE(&frame[1], 4);
    writeU32LE(&frame[3], since_seq);
    return frame;
}

inline std::vector<uint8_t> buildSetFieldCmdFrame(uint8_t comp_id, uint8_t field_tag, const uint8_t* val, uint8_t val_len) {
    std::vector<uint8_t> frame(HEADER_SIZE + 3 + val_len);
    frame[0] = static_cast<uint8_t>(MsgType::CMD_SET_FIELD);
    writeU16BE(&frame[1], static_cast<uint16_t>(3 + val_len));
    frame[3] = comp_id;
    frame[4] = field_tag;
    frame[5] = val_len;
    if (val_len > 0 && val != nullptr) {
        std::memcpy(&frame[6], val, val_len);
    }
    return frame;
}

inline std::vector<uint8_t> buildExecActionCmdFrame(uint8_t cmd_id, uint32_t nonce, uint32_t param, const char* data = nullptr) {
    size_t data_len = data ? std::strlen(data) : 0;
    if (data_len > 128) data_len = 128;
    std::vector<uint8_t> frame(HEADER_SIZE + 10 + data_len);
    frame[0] = static_cast<uint8_t>(MsgType::CMD_EXEC_ACTION);
    writeU16BE(&frame[1], static_cast<uint16_t>(10 + data_len));
    frame[3] = cmd_id;
    writeU32LE(&frame[4], nonce);
    writeU32LE(&frame[8], param);
    frame[12] = static_cast<uint8_t>(data_len);
    if (data_len > 0) {
        std::memcpy(&frame[13], data, data_len);
    }
    return frame;
}

} // namespace StarProtocol
