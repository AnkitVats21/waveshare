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
        NORMAL           = 5,
        KEY_POLL         = 5,   ///< KeyService polling loop
        AUDIO_ALERT      = 5,   ///< Fire-and-forget audio alert tasks
        MQTT             = 5,   ///< MqttService background loop
        ASSISTANT        = 6,   ///< AssistantService state machine
        LED              = 6,   ///< LedService animation loop
        GEMINI_PROTOCOL  = 7,   ///< GeminiProtocol WebSocket handler (Core 0)
        AUDIO_PUMP       = 8,   ///< GeminiAudioPump uplink (Core 1)
        AUDIO_SERVICE    = 10,  ///< AudioService reactor loop
        SPEAKER_PLAYBACK = 14,  ///< SpeakerPlayback I2S write task
        MIC_CAPTURE      = 14,  ///< MicCaptureTask I2S read task
        WAKE_WORD_DETECT = 18,  ///< WakeWordEngine detect task (AFE fetch)
        WAKE_WORD_FEED   = 19,  ///< WakeWordEngine feed task  (I2S read)
    };

    // ── Stack sizes (bytes) ────────────────────────────────────────────────
    enum StackSize : uint32_t {
        STACK_SMALL   = 3 * 1024,
        STACK_NORMAL  = 4 * 1024,
        STACK_MEDIUM  = 6 * 1024,
        STACK_LARGE   = 8 * 1024,
        STACK_GEMINI  = 4 * 1024,  ///< GeminiProtocol (cJSON + WssClient)
        STACK_WW_FEED = 8 * 1024,
        STACK_WW_DET  = 8 * 1024,
    };

    // ── Core affinities ────────────────────────────────────────────────────
    static constexpr BaseType_t CORE_NETWORK = 0; ///< WiFi, WebSocket, MQTT
    static constexpr BaseType_t CORE_AUDIO   = 1; ///< I2S DMA, AFE, audio pump
    static constexpr BaseType_t CORE_ANY     = tskNO_AFFINITY;

} // namespace ThreadConfig
