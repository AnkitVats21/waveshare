#pragma once

#include <cmath>
#include <cstdint>

namespace App {

// Mic gain 0-100 dB. The ES7210's analog PGA stops at 37.5 dB (the codec
// driver maps anything from 37 dB up to 37.5), so the part above it is a
// digital gain applied to the mic samples after the read. Digital gain
// raises noise and echo with the speech, and loud sounds clip (saturate).
namespace MicGain {
constexpr float MIN_DB = 0.0f;
constexpr float MAX_DB = 100.0f;
constexpr float ANALOG_MAX_DB = 37.5f;

inline float clampDb(float db) {
    if (!(db >= MIN_DB)) return MIN_DB;  // also catches NaN
    return db > MAX_DB ? MAX_DB : db;
}

// The part the codec applies.
inline float analogDb(float db) {
    db = clampDb(db);
    return db > ANALOG_MAX_DB ? ANALOG_MAX_DB : db;
}

// The linear factor applied in software (1.0 up to 37.5 dB).
inline float digitalFactor(float db) {
    float extra = clampDb(db) - ANALOG_MAX_DB;
    return extra > 0.0f ? std::pow(10.0f, extra / 20.0f) : 1.0f;
}

// Multiply one sample, saturating to int16.
inline int16_t apply(int16_t s, float factor) {
    float v = s * factor;
    if (v >= 32767.0f) return 32767;
    if (v <= -32768.0f) return -32768;
    return (int16_t)std::lround(v);
}
}  // namespace MicGain

}  // namespace App
