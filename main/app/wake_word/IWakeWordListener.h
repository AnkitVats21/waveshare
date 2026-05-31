#pragma once

#include <cstdint>

/**
 * @file IWakeWordListener.h
 * @brief Callback interface delivered by WakeWordDetector to its consumer.
 *
 * AudioService implements this interface. The detector has no knowledge
 * of EventBus, Board, or any other application-layer construct — it only
 * calls these two methods.
 */
class IWakeWordListener {
public:
    /**
     * @brief Called when a wake word is confirmed.
     * @param channel  AFE beamforming channel index that triggered the event.
     */
    virtual void onWakeWord(uint8_t channel) = 0;

    /**
     * @brief Called when VAD silence timeout expires (user stopped speaking).
     *
     * The detector re-arms WakeNet automatically before calling this.
     */
    virtual void onVadTimeout() = 0;

    /**
     * @brief Called when user speech is detected while assistant is active
     *        (barge-in / interruption).
     */
    virtual void onUserSpeechDetected() = 0;

    virtual ~IWakeWordListener() = default;
};
