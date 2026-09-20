#pragma once

#include "core_sysdb/SystemState.h"
#include "core_sysdb/WalTypes.h"
#include "core_sysdb/WalRingBuffer.h"
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace SysDbCodec {

template<typename T>
inline uint8_t serializeValue(const T& val, uint8_t* out, size_t max_out) {
    if (sizeof(T) > max_out) return 0;
    std::memcpy(out, &val, sizeof(T));
    return static_cast<uint8_t>(sizeof(T));
}

inline uint8_t serializeString(const char* str, size_t max_len, uint8_t* out, size_t max_out) {
    size_t len = ::strnlen(str, max_len);
    if (len > max_out) len = max_out;
    std::memcpy(out, str, len);
    return static_cast<uint8_t>(len);
}

inline uint8_t serializeRgbColor(const RgbColor& color, uint8_t* out, size_t max_out) {
    if (max_out < 3) return 0;
    out[0] = color.r;
    out[1] = color.g;
    out[2] = color.b;
    return 3;
}

inline uint8_t serializeCommand(const MediaPendingCommand& cmd, uint8_t* out, size_t max_out) {
    size_t data_len = ::strnlen(cmd.data, sizeof(cmd.data));
    size_t total = 1 + 4 + 4 + 1 + data_len;
    if (total > max_out) return 0;
    out[0] = static_cast<uint8_t>(cmd.cmd);
    std::memcpy(out + 1, &cmd.nonce, 4);
    std::memcpy(out + 5, &cmd.param, 4);
    out[9] = static_cast<uint8_t>(data_len);
    if (data_len > 0) std::memcpy(out + 10, cmd.data, data_len);
    return static_cast<uint8_t>(total);
}

template<typename T>
inline uint8_t serializeAny(const T& val, uint8_t* out, size_t max_out) {
    if constexpr (std::is_same_v<T, MediaPendingCommand>) {
        return serializeCommand(val, out, max_out);
    } else {
        return serializeValue(val, out, max_out);
    }
}

template<typename T>
inline bool deserializeValue(T& out_val, const uint8_t* in, uint8_t len) {
    if (len != sizeof(T) || in == nullptr) return false;
    std::memcpy(&out_val, in, sizeof(T));
    return true;
}

inline bool deserializeString(char* out_str, size_t buf_size, const uint8_t* in, uint8_t len) {
    if (buf_size == 0) return false;
    size_t copy_len = len < buf_size ? len : buf_size - 1;
    if (copy_len > 0 && in != nullptr) {
        std::memcpy(out_str, in, copy_len);
    }
    out_str[copy_len] = '\0';
    return true;
}

inline bool deserializeRgbColor(RgbColor& out_color, const uint8_t* in, uint8_t len) {
    if (len != 3 || in == nullptr) return false;
    out_color.r = in[0];
    out_color.g = in[1];
    out_color.b = in[2];
    return true;
}

inline bool deserializeCommand(MediaPendingCommand& out_cmd, const uint8_t* in, uint8_t len) {
    if (len < 10 || in == nullptr) return false;
    out_cmd.cmd = static_cast<MediaCmdId>(in[0]);
    std::memcpy(&out_cmd.nonce, in + 1, 4);
    std::memcpy(&out_cmd.param, in + 5, 4);
    uint8_t data_len = in[9];
    if (len < 10 + data_len) return false;
    size_t copy_len = data_len < sizeof(out_cmd.data) ? data_len : sizeof(out_cmd.data) - 1;
    if (copy_len > 0) {
        std::memcpy(out_cmd.data, in + 10, copy_len);
    }
    out_cmd.data[copy_len] = '\0';
    return true;
}

template<typename T>
inline bool deserializeAny(T& val, const uint8_t* in, uint8_t len) {
    if constexpr (std::is_same_v<T, MediaPendingCommand>) {
        return deserializeCommand(val, in, len);
    } else {
        return deserializeValue(val, in, len);
    }
}

/**
 * @brief Apply a deserialized write to a specific field in SystemState.
 */
bool applyFieldWrite(SystemState& state, ComponentId comp, uint8_t field_tag, const uint8_t* val, uint8_t len);

/**
 * @brief Diff old and new state, emit WAL records for changes into ring buffer, and compute ComponentMask.
 */
ComponentMask diffAndEmitWal(const SystemState& old_s, const SystemState& new_s, WalRingBuffer& wal);

/**
 * @brief Serialize all fields in SystemState into a snapshot record array.
 */
void serializeSnapshot(const SystemState& state, uint32_t head_seq, std::vector<WalRecordEntry>& out_snapshot);

} // namespace SysDbCodec
