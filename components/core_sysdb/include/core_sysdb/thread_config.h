#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * @brief Centralized FreeRTOS task priority and stack-size constants.
 *
 * All task priorities are defined here to prevent accidental conflicts.
 * Higher numeric value = higher priority. IDLE = 0, max = configMAX_PRIORITIES-1.
 *
 * Core affinity conventions:
 *   Core 0: Network I/O (WiFi tx/rx, WebSocket, MQTT)
 *   Core 1: Audio DSP  (I2S DMA, AFE feed/detect, audio pump)
 */
namespace ThreadConfig {

    // ── Priority tiers ─────────────────────────────────────────────────────
    enum Priority : UBaseType_t {
        IDLE             = 0,
        LOW              = 2,
        STORAGE_IO       = 3,   ///< Low-priority SD card disk I/O (Core 0)
        NORMAL           = 5,
        KEY_POLL         = 5,   ///< KeyService polling loop
        MQTT             = 5,   ///< MqttService background loop
        ASSISTANT        = 6,   ///< AssistantService state machine
        LED              = 6,   ///< LedService animation loop
        GEMINI_PROTOCOL  = 7,   ///< GeminiProtocol WebSocket handler (Core 0)
        AUDIO_PUMP       = 8,   ///< GeminiAudioPump uplink (Core 1)
        WAKE_WORD_FEED   = 9,   ///< WakeWordEngine feed task (DSP AFE on Core 1)
        AUDIO_SERVICE    = 10,  ///< AudioService reactor loop
        AUDIO_ALERT      = 11,  ///< AlertPlayer chime/notification generator (Core 1)
        WAKE_WORD_DETECT = 12,  ///< WakeWordEngine detect/fetch task
        MIC_CAPTURE      = 14,  ///< MicCaptureTask I2S read task
        SPEAKER_PLAYBACK = 18,  ///< SpeakerPlayback I2S write task
    };

    // ── Stack sizes (bytes) ────────────────────────────────────────────────
    enum StackSize : uint32_t {
        STACK_SMALL     = 3 * 1024,
        STACK_NORMAL    = 4 * 1024,  ///< Bumped from 3K — STACK_SMALL alias was too tight
        STACK_MEDIUM    = 6 * 1024,
        STACK_STORAGE   = 4 * 1024,  ///< SD Card disk writer and reader tasks
        STACK_LARGE     = 8 * 1024,
        STACK_ASSISTANT = 6 * 1024,  ///< AssistantService — transitionTo() frames carry a SystemState copy
        STACK_GEMINI    = 8 * 1024,  ///< GeminiProtocol — needs room for ArduinoJson tool-call serialization
        STACK_PLAYER    = 12 * 1024, ///< NexusPlayer — handles InvidiousClient HTTPS + ArduinoJson during autoplay
        STACK_WW_FEED   = 3 * 1024,
        STACK_WW_DET    = 8 * 1024,
    };

    // ── Core affinities ────────────────────────────────────────────────────
    static constexpr BaseType_t CORE_NETWORK = 0; ///< WiFi, WebSocket, MQTT
    static constexpr BaseType_t CORE_STORAGE = 0; ///< SD Card disk I/O (Core 0, off Core 1 DSP)
    static constexpr BaseType_t CORE_AUDIO   = 1; ///< I2S DMA, AFE, audio pump
    static constexpr BaseType_t CORE_ANY     = tskNO_AFFINITY;

} // namespace ThreadConfig
