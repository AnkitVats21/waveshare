#pragma once

#include <cstdint>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// SysDb core types shared by the generated schema
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
 * @brief Sequential, compact component identifiers.
 */
enum class ComponentId : uint8_t {
    SYSTEM    = 0,
    AUDIO     = 1,
    PIPELINE  = 2,
    ASSISTANT = 3,
    LED       = 4,
    // 5 was MQTT; retired.
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
