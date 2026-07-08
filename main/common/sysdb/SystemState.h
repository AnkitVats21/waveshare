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
    static constexpr ComponentMask SYSTEM    = (1u << 16); ///< WiFi, server IP
    static constexpr ComponentMask AUDIO     = (1u << 17); ///< Sample rate, volume, mic
    static constexpr ComponentMask PIPELINE  = (1u << 18); ///< PipelineMode, RTP gates
    static constexpr ComponentMask ASSISTANT = (1u << 19); ///< Session + visual state
    static constexpr ComponentMask LED       = (1u << 20); ///< LED animation commands
    static constexpr ComponentMask MQTT      = (1u << 21); ///< Broker connectivity
    static constexpr ComponentMask ALARM     = (1u << 22); ///< Active alarm status/control
    static constexpr ComponentMask ALL       = 0xFFFF0000u;
}

// Per-field bits — used by onStateChanged() for fine-grained reactions
namespace BIT_ALARM {
    static constexpr ComponentMask PLAYING        = (1u << 0);
    static constexpr ComponentMask STOP_REQUESTED = (1u << 1);
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
    static constexpr ComponentMask WAV_PLAYING    = (1u << 9);
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

#define SYSTEM_FIELDS \
    X(bool, wifi_connected, false, BIT_SYSTEM::WIFI_CONNECTED) \
    X_STR(server_ip, 32, "192.168.1.18", BIT_SYSTEM::SERVER_IP) \
    X(int, wifi_max_retries, 5, 0)

#define AUDIO_FIELDS \
    X(uint32_t, sample_rate, 24000, BIT_AUDIO::SAMPLE_RATE) \
    X(uint32_t, current_hardware_rate, 24000, BIT_AUDIO::HW_RATE) \
    X(float, mic_gain_db, 60.0f, BIT_AUDIO::MIC_GAIN) \
    X(int, speaker_volume, 80, BIT_AUDIO::SPEAKER_VOLUME) \
    X(bool, mic_enabled, true, BIT_AUDIO::MIC_ENABLED) \
    X(bool, assistant_speaking, false, BIT_AUDIO::ASST_SPEAKING) \
    X(bool, session_active, false, BIT_AUDIO::SESSION_ACTIVE) \
    X(bool, turn_complete_pending, false, BIT_AUDIO::TURN_COMPLETE) \
    X(uint64_t, last_activity_ms, 0, BIT_AUDIO::LAST_ACTIVITY) \
    X(uint32_t, buffer_size, 131072, 0) \
    X(uint16_t, rtp_tx_port, 5005, 0) \
    X(uint16_t, rtp_rx_port, 5005, 0) \
    X(AudioStreamFormat, stream_format, AudioStreamFormat::PCM_S16LE, 0) \
    X(bool, wav_playing, false, BIT_AUDIO::WAV_PLAYING) \
    X(uint32_t, wav_sample_rate, 16000, 0) \
    X(bool, wav_prefetched, false, 0)

#define PIPELINE_FIELDS \
    X(PipelineMode, mode, PipelineMode::WAKE_IDLE, BIT_PIPELINE::MODE) \
    X(bool, rtp_tx_en, false, BIT_PIPELINE::RTP_TX) \
    X(bool, rtp_rx_en, false, BIT_PIPELINE::RTP_RX) \
    X(bool, rtp_enabled, false, 0)

#define ASSISTANT_FIELDS \
    X(AssistantState, session_state, AssistantState::Idle, BIT_ASSISTANT::SESSION_STATE) \
    X(AssistantVisualState, visual_state, AssistantVisualState::Offline, BIT_ASSISTANT::VISUAL_STATE) \
    X(bool, connect_requested, false, BIT_ASSISTANT::CONNECT_REQ) \
    X(WsState, ws_state, WsState::DISCONNECTED, BIT_ASSISTANT::WS_STATE) \
    X(bool, mpv_pending_idle, false, 0)

#define LED_FIELDS \
    X(LedMode, mode, LedMode::OFF, BIT_LED::MODE) \
    X_COLOR(color, OFF_LED, BIT_LED::COLOR) \
    X(uint32_t, speed_ms, 500, BIT_LED::SPEED) \
    X(uint8_t, repeat, 0, 0)

#define MQTT_FIELDS \
    X(bool, connected, false, BIT_MQTT::CONNECTED)

#define ALARM_FIELDS \
    X(bool, playing, false, BIT_ALARM::PLAYING) \
    X(bool, stop_requested, false, BIT_ALARM::STOP_REQUESTED) \
    X(int, active_alarm_id, 0, 0)


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
    #define X(type, name, def, bit) type name = def;
    #define X_STR(name, size, def, bit) char name[size] = def;
    #define X_COLOR(name, def, bit) RgbColor name = def;

    // ── COMP::SYSTEM ─────────────────────────────────────────────────────────
    struct {
        SYSTEM_FIELDS
    } system;

    // ── COMP::AUDIO ──────────────────────────────────────────────────────────
    struct {
        AUDIO_FIELDS
    } audio;

    // ── COMP_PIPELINE ───────────────────────────────────
    struct {
        PIPELINE_FIELDS
    } pipeline;

    // ── COMP::ASSISTANT ──────────────────────────────────────────────────────
    struct {
        ASSISTANT_FIELDS
    } assistant;

    // ── COMP::LED ────────────────────────────────────────────────────────────
    struct {
        LED_FIELDS
    } led;

    // ── COMP::MQTT ───────────────────────────────────────────────────────────
    struct {
        MQTT_FIELDS
    } mqtt;

    // ── COMP::ALARM ──────────────────────────────────────────────────────────
    struct {
        ALARM_FIELDS
    } alarm;


    #undef X
    #undef X_STR
    #undef X_COLOR
};
