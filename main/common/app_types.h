#pragma once

/**
 * @file app_types.h
 * @brief Unified application-level enum types.
 *
 * This is the single source of truth for all high-level state enums.
 * All components include this file instead of each other's headers.
 *
 * Includes:
 *   - AssistantState       — conversation session state machine
 *   - AssistantVisualState — LED orchestration state
 *   - PipelineMode         — active audio backend selector
 *   - AudioStreamFormat    — wire format for audio streaming
 *
 * Hardware handle types → common/hw_types.h
 * LED color/animation    → common/led_types.h
 * Event bases            → common/events/event_bases.h  (legacy, being removed)
 */

#include "common/led_types.h"
#include <cstdint>

// ── Assistant conversation state machine ─────────────────────────────────────
enum class AssistantState : int {
    Idle,               ///< WakeNet armed, no session
    StartingSession,    ///< Triggered, spinning up connection
    Connecting,         ///< WebSocket/RTP connect in progress
    StreamingUserAudio, ///< Live mic→cloud streaming
    AssistantSpeaking,  ///< Cloud audio playing back
    WaitingForFollowup, ///< Turn complete, holding session open
    Closing,            ///< Graceful session tear-down
    ErrorCooldown,      ///< Back-off after failure
};

// ── Visual state for LED orchestration ───────────────────────────────────────
enum class AssistantVisualState : int {
    Idle,           ///< Quiet breathing / off
    Listening,      ///< Wake word confirmed — user speaking
    Connecting,     ///< Network handshake in progress
    Speaking,       ///< Assistant audio playing
    Thinking,       ///< Waiting for first server response
    Offline,        ///< No WiFi / MQTT
    Recovering,     ///< Attempting reconnection
    RateLimited,    ///< 429 back-off period
    Error,          ///< Hard error state
};

// ── Audio pipeline mode selector ─────────────────────────────────────────────
enum class PipelineMode : uint8_t {
    WAKE_IDLE,    ///< WakeNet armed, no network streaming (boot default)
    GEMINI_LIVE,  ///< WebSocket uplink via GeminiAudioPump
    RTP_REMOTE,   ///< UDP RTP Tx/Rx to remote inference server
    RTP_WAKEWORD, ///< Wake word over RTP + UDP control signals
};

// ── Audio wire format ─────────────────────────────────────────────────────────
enum class AudioStreamFormat : uint8_t {
    PCM_S16LE, ///< Raw 16-bit signed little-endian PCM
    G711_ULAW, ///< 8-bit u-law compressed
};

// ── WebSocket Connection State ───────────────────────────────────────────────
enum class WsState : uint8_t {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    GOING_AWAY,
    ERROR_STATE,
};

