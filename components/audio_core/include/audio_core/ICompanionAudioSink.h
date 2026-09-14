#pragma once
#include <cstdint>
#include <cstddef>

/**
 * @brief Abstract sink interface for companion audio (e.g. BT A2DP via ESP32-WROOM).
 * Decouples audio_core from the hal/companion hardware driver.
 */
class ICompanionAudioSink {
public:
    virtual ~ICompanionAudioSink() = default;
    virtual bool isInitialized() const = 0;
    virtual size_t writeSamples(const int16_t* samples, size_t frames, uint32_t timeout_ms) = 0;
};
