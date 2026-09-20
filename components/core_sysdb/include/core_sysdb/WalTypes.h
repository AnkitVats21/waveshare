#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// STAR Replication Core Types
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Access permissions for SystemState fields.
 */
enum class FieldAccess : uint8_t {
    ReadOnly  = 0, ///< Hardware/OS telemetry (reject remote write requests)
    Writable  = 1, ///< Settable scalar state (Pi, web dashboard, or local UI can request)
    PiOrigin  = 2  ///< State observed exclusively by Raspberry Pi (e.g. bt.connected)
};

/**
 * @brief Sequential, compact component identifiers for wire replication.
 */
enum class ComponentId : uint8_t {
    SYSTEM    = 0,
    AUDIO     = 1,
    PIPELINE  = 2,
    ASSISTANT = 3,
    LED       = 4,
    // 5 was MQTT; retired (no live component ever used it). Left unassigned
    // rather than renumbering ALARM..MEDIA, since this byte value is also
    // the wire-protocol id shared with the starhub daemon's vendored copy
    // of this header.
    ALARM     = 6,
    BLUETOOTH = 7,
    MEDIA     = 8,
    COUNT     = 9
};

/**
 * @brief Active media playback audio output target.
 */
enum class MediaOutputTarget : uint8_t {
    LOCAL = 0, ///< Waveshare onboard speaker via WebMOpusDecoder
    PI_BT = 1  ///< Raspberry Pi Zero 2W MPD -> Tribit Bluetooth speaker
};

/**
 * @brief Named media control commands dispatched through SystemState.
 */
enum class MediaCmdId : uint8_t {
    NONE     = 0,
    PLAY     = 1,
    PAUSE    = 2,
    RESUME   = 3,
    STOP     = 4,
    NEXT     = 5,
    PREVIOUS = 6,
    SEEK     = 7,
    AUTOPLAY = 8,
    CACHING  = 9,
    VOLUME   = 10
};

/**
 * @brief Pending command struct stored in COMP::MEDIA.
 */
struct MediaPendingCommand {
    MediaCmdId cmd = MediaCmdId::NONE;
    uint32_t   nonce = 0;       ///< Incremented monotonically on every new command
    uint32_t   param = 0;       ///< e.g. seek position in ms or volume level (0..100)
    char       data[128] = {0}; ///< e.g. stream URL or track ID / search query

    bool operator==(const MediaPendingCommand& o) const {
        return cmd == o.cmd && nonce == o.nonce && param == o.param &&
               std::strncmp(data, o.data, sizeof(data)) == 0;
    }

    bool operator!=(const MediaPendingCommand& o) const {
        return !(*this == o);
    }
};

/**
 * @brief Result of a remote write request through the SysDb write-gate.
 */
enum class WriteResult : uint8_t {
    OK                = 0, ///< Mutation accepted, applied, and emitted to WAL
    REJECTED_READONLY = 1, ///< Field is marked ReadOnly, structurally immutable
    INVALID_COMPONENT = 2, ///< Component ID not found
    INVALID_TAG       = 3, ///< Field tag out of range for component
    DECODE_ERROR      = 4  ///< Value bytes could not be decoded into target type
};

/**
 * @brief Status of a sequence catch-up query.
 */
enum class WalQueryResult : uint8_t {
    SUCCESS           = 0, ///< All records since requested seq are returned
    SNAPSHOT_REQUIRED = 1, ///< Requested seq has scrolled past ring; full snapshot required
    UP_TO_DATE        = 2  ///< Requested seq matches current head; no new records
};

/**
 * @brief Wire representation of a single WAL mutation record.
 */
struct __attribute__((packed)) WalWireRecord {
    uint32_t seq;          ///< Monotonically increasing sequence number
    uint8_t  component_id; ///< ComponentId as uint8_t
    uint8_t  field_tag;    ///< Sequential field index within component
    uint8_t  val_len;      ///< Length of value payload in bytes
    uint8_t  value[];      ///< Raw serialized value bytes
};

/**
 * @brief High-level C++ representation of a WAL record.
 */
struct WalRecordEntry {
    uint32_t seq = 0;
    uint8_t  component_id = 0;
    uint8_t  field_tag = 0;
    std::vector<uint8_t> value;
};
