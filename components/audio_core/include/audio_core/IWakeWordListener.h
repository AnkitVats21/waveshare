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
     * @brief Asked when the silence timeout expires, before streaming stops.
     *
     * False keeps the session streaming and restarts the silence count (a
     * reply is still owed, or the assistant is about to speak).
     */
    virtual bool mayEndOnSilence() { return true; }

    /**
     * @brief Called when the wake word is heard over the assistant's reply
     *        (barge-in on): the reply should stop.
     */
    virtual void onUserSpeechDetected() = 0;

    /**
     * @brief Called when speech activity is detected during streaming.
     */
    virtual void onSpeechDetected() = 0;

    virtual ~IWakeWordListener() = default;
};
