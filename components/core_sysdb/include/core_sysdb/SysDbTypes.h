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
    SATELLITE = 1  ///< a PC or Pi running nexus-orbit (main/services/http/OrbitChannel.h)
};

