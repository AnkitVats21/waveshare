#pragma once

#include <cstdint>

/**
 * @brief Synthesizes short PCM tones and injects them into the SPK_RX_BUF ring
 * buffer so they play through the existing SpeakerPlaybackTask pipeline.
 *
 * Tones are generated at 16kHz, 16-bit mono (the standard wake/idle pipeline
 * sample rate). When the assistant is playing back audio at 24kHz the tones
 * will be pitched up by ~1.5x but remain clearly audible — this is acceptable
 * for alert UX.
 *
 * All methods are static and non-blocking; they return immediately after
 * queueing samples into the ring buffer.
 */
class AudioAlertPlayer {
public:
    /**
     * @brief Short rising two-tone chime — played when wake-word is confirmed.
     *        Signals the user that the assistant heard them.
     */
    static void playWakeConfirm();

    /**
     * @brief Smooth ascending ding — played when the WebSocket is connected
     *        and the microphone is live. Tells the user "speak now".
     */
    static void playReadyToSpeak();

    /**
     * @brief Soft descending tone — played when the session closes gracefully.
     */
    static void playSessionEnd();

    /**
     * @brief Short double-blip error tone — played on connection timeout or
     *        quota / server errors.
     */
    static void playError();

    /**
     * @brief Short single-click — played when Wi-Fi goes offline while the
     *        user attempts to use the assistant.
     */
    static void playOffline();

private:
    // Sample rate assumed by the speaker pipeline at wake/idle time
    static constexpr int SAMPLE_RATE = 16000;

    /**
     * @brief Internal helper: generate a pure sine tone and push it into the
     * ring buffer. Uses only stack memory (no heap allocation).
     *
     * @param freq_hz   Tone frequency in Hz
     * @param volume    Peak amplitude [0 – 32767]
     * @param duration_ms  Duration in milliseconds
     * @param fade_ms   Fade-in AND fade-out window in milliseconds (prevents clicks)
     */
    static void playTone(float freq_hz, int16_t volume,
                         uint32_t duration_ms, uint32_t fade_ms = 10);
};
