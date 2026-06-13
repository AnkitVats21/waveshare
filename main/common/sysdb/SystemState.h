#pragma once

#include "common/app_types.h"
#include "common/led_types.h"
#include <cstdint>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Component-level change bitmasks
// ─────────────────────────────────────────────────────────────────────────────

using ComponentMask = uint32_t;

/**
 * @brief Top-level component identifiers.
 *
 * Pass one or more OR'd COMP:: values to EmbeddedSysDb::mutate() and
 * ReactorTask::Config::interest to declare what a reactor cares about.
 */
namespace COMP {
    static constexpr ComponentMask SYSTEM    = (1u << 0); ///< WiFi, server IP
    static constexpr ComponentMask AUDIO     = (1u << 1); ///< Sample rate, volume, mic
    static constexpr ComponentMask PIPELINE  = (1u << 2); ///< PipelineMode, RTP gates
    static constexpr ComponentMask ASSISTANT = (1u << 3); ///< Session + visual state
    static constexpr ComponentMask LED       = (1u << 4); ///< LED animation commands
    static constexpr ComponentMask MQTT      = (1u << 5); ///< Broker connectivity
    static constexpr ComponentMask ALL       = 0xFFFFFFFFu;
}

// Per-field bits — used by onStateChanged() for fine-grained reactions
namespace BIT_SYSTEM {
    static constexpr ComponentMask WIFI_CONNECTED = (1u << 0);
    static constexpr ComponentMask SERVER_IP      = (1u << 1);
}
namespace BIT_AUDIO {
    static constexpr ComponentMask SAMPLE_RATE    = (1u << 0);
    static constexpr ComponentMask MIC_GAIN       = (1u << 1);
    static constexpr ComponentMask SPEAKER_VOLUME = (1u << 2);
    static constexpr ComponentMask MIC_ENABLED    = (1u << 3);
    static constexpr ComponentMask ASST_SPEAKING  = (1u << 4);
    static constexpr ComponentMask SESSION_ACTIVE = (1u << 5);
    static constexpr ComponentMask TURN_COMPLETE  = (1u << 6);
    static constexpr ComponentMask HW_RATE        = (1u << 7);
    static constexpr ComponentMask LAST_ACTIVITY  = (1u << 8);
}
namespace BIT_PIPELINE {
    static constexpr ComponentMask MODE           = (1u << 0);
    static constexpr ComponentMask RTP_TX         = (1u << 1);
    static constexpr ComponentMask RTP_RX         = (1u << 2);
}
namespace BIT_ASSISTANT {
    static constexpr ComponentMask SESSION_STATE  = (1u << 0);
    static constexpr ComponentMask VISUAL_STATE   = (1u << 1);
    static constexpr ComponentMask WS_STATE       = (1u << 2);
    static constexpr ComponentMask CONNECT_REQ    = (1u << 3);
}
namespace BIT_LED {
    static constexpr ComponentMask MODE           = (1u << 0);
    static constexpr ComponentMask COLOR          = (1u << 1);
    static constexpr ComponentMask SPEED          = (1u << 2);
}
namespace BIT_MQTT {
    static constexpr ComponentMask CONNECTED      = (1u << 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// SystemState — the single shared in-memory database
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Complete, flat snapshot of all mutable application state.
 *
 * All fields are plain data (no mutexes, no atomics).  Thread safety is
 * provided entirely by EmbeddedSysDb's reader-writer lock.
 *
 * Rules:
 *  - Only EmbeddedSysDb::mutate() may write to this struct.
 *  - Readers call EmbeddedSysDb::snapshot() to get a value-copy.
 *  - Hot-path readers may use the typed getter helpers on EmbeddedSysDb.
 */
struct SystemState {

    // ── COMP::SYSTEM ─────────────────────────────────────────────────────────
    struct {
        bool        wifi_connected   = false;
        std::string server_ip        = "192.168.1.18";
        std::string wifi_ssid;
        std::string wifi_password;
        int         wifi_max_retries = 5;
    } system;

    // ── COMP::AUDIO ──────────────────────────────────────────────────────────
    struct {
        uint32_t          sample_rate           = 16000;
        uint32_t          current_hardware_rate = 16000;
        float             mic_gain_db           = 60.0f;
        int               speaker_volume        = 80;
        bool              mic_enabled           = true;
        bool              assistant_speaking    = false;
        bool              session_active        = false;
        bool              turn_complete_pending = false;
        uint64_t          last_activity_ms      = 0;

        // Boot-time Kconfig values — future: overridable via MQTT
        uint32_t          buffer_size           = 131072;
        uint16_t          rtp_tx_port           = 5005;
        uint16_t          rtp_rx_port           = 5005;
        AudioStreamFormat stream_format         = AudioStreamFormat::PCM_S16LE;
    } audio;

    // ── COMP_PIPELINE ───────────────────────────────────
    struct {
        PipelineMode mode           = PipelineMode::WAKE_IDLE;
        bool         rtp_tx_en      = false;
        bool         rtp_rx_en      = false;
        bool         rtp_enabled    = false; // Unified gate for RtpTransceiver
    } pipeline;

    // ── COMP::ASSISTANT ──────────────────────────────────────────────────────
    struct {
        AssistantState       session_state    = AssistantState::Idle;
        AssistantVisualState visual_state     = AssistantVisualState::Offline;
        bool                 connect_requested = false;
        WsState              ws_state          = WsState::DISCONNECTED;
    } assistant;

    // ── COMP::LED ────────────────────────────────────────────────────────────
    struct {
        LedMode  mode     = LedMode::OFF;
        RgbColor color    = OFF_LED;
        uint32_t speed_ms = 500;
        uint8_t  repeat   = 0;
    } led;

    // ── COMP::MQTT ───────────────────────────────────────────────────────────
    struct {
        bool connected = false;
    } mqtt;
};
