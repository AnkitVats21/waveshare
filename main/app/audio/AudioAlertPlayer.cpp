#include "AudioAlertPlayer.h"
#include "services/BufferManager.h"
#include "app/audio/SpeakerPlayback.h"   // for DECLARE_BUFFER / SPK_RX_BUF id
#include "common/AppLogger.h"
#include "freertos/FreeRTOS.h"
#include <cmath>
#include <cstring>

// ---------------------------------------------------------------------------
// Internal tone synthesiser
// ---------------------------------------------------------------------------
// Stack budget: 16 kHz × 0.5 s × 2 bytes = 16 kB max per call.
// We keep individual notes short (≤ 300 ms) so the stack requirement is
// comfortably below 10 kB.
// ---------------------------------------------------------------------------

void AudioAlertPlayer::playTone(float freq_hz, int16_t volume,
                                uint32_t duration_ms, uint32_t fade_ms) {
    const uint32_t total_samples = (SAMPLE_RATE * duration_ms) / 1000;
    const uint32_t fade_samples  = (SAMPLE_RATE * fade_ms) / 1000;

    // Stack-local scratch (max ~600 bytes for a 300-ms chunk at 16kHz)
    // We chunk into 128-sample blocks to keep the stack tiny.
    constexpr uint32_t BLOCK = 128;
    int16_t buf[BLOCK];

    uint32_t sent = 0;
    while (sent < total_samples) {
        const uint32_t n = (total_samples - sent < BLOCK) ? (total_samples - sent) : BLOCK;

        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t s = sent + i;
            float env = 1.0f;

            // Linear fade-in
            if (s < fade_samples && fade_samples > 0) {
                env = (float)s / (float)fade_samples;
            }
            // Linear fade-out
            else if (s >= (total_samples - fade_samples) && fade_samples > 0) {
                env = (float)(total_samples - s) / (float)fade_samples;
            }

            const float angle = 2.0f * 3.14159265f * freq_hz * (float)s / (float)SAMPLE_RATE;
            buf[i] = (int16_t)(env * (float)volume * sinf(angle));
        }

        // Push into the speaker ring buffer (10 ms timeout — non-blocking in spirit)
        BufferManager::getInstance().send(
            Buffers::SPK_RX_BUF,
            reinterpret_cast<uint8_t*>(buf),
            n * sizeof(int16_t),
            pdMS_TO_TICKS(10));

        sent += n;
    }
}

// ---------------------------------------------------------------------------
// Public alert sequences
// ---------------------------------------------------------------------------

// Wake confirm: two quick rising tones  (G5 → B5, 80 ms each)
void AudioAlertPlayer::playWakeConfirm() {
    playTone(784.0f, 8000, 80, 15);   // G5
    playTone(987.8f, 8000, 80, 15);   // B5
}

// Ready to speak: ascending major arpeggio (C5 → E5 → G5, 90 ms each)
// This is the most important cue — tells the user the mic is hot.
void AudioAlertPlayer::playReadyToSpeak() {
    playTone(523.3f, 10000, 90, 15);  // C5
    playTone(659.3f, 10000, 90, 15);  // E5
    playTone(784.0f, 10000, 130, 20); // G5 (held slightly longer)
}

// Session end: descending minor third  (E5 → C5, 100 ms each)
void AudioAlertPlayer::playSessionEnd() {
    playTone(659.3f, 7000, 100, 20);  // E5
    playTone(523.3f, 7000, 120, 20);  // C5
}

// Error: rapid double-blip (A4, 60 ms × 2 with 40 ms gap)
void AudioAlertPlayer::playError() {
    playTone(440.0f, 9000, 60, 10);
    // gap — send silence for 40 ms
    {
        constexpr uint32_t GAP_SAMPLES = (SAMPLE_RATE * 40) / 1000; // samples for 40ms gap
        int16_t silence[GAP_SAMPLES] = {};
        BufferManager::getInstance().send(
            Buffers::SPK_RX_BUF,
            reinterpret_cast<uint8_t*>(silence),
            GAP_SAMPLES * sizeof(int16_t),
            pdMS_TO_TICKS(10));
    }
    playTone(440.0f, 9000, 60, 10);
}

// Offline: single dull low thud (D3, 120 ms)
void AudioAlertPlayer::playOffline() {
    playTone(146.8f, 8000, 120, 25);  // D3
}
