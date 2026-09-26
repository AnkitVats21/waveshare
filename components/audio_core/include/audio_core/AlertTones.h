#pragma once

#include "audio_core/AlertMixer.h"

// One note of a built-in chime; hz = 0 is a silent gap.
struct ToneNote {
    float hz;
    int16_t amplitude;
    uint16_t ms;
    uint16_t fade_ms;
};

// Renders a sequence of sine notes, each with a linear fade in and out, into
// a new clip at sample_rate. Returns nullptr if PSRAM is exhausted.
std::shared_ptr<AlertClip> synthesizeTones(const ToneNote* notes, size_t count, uint32_t sample_rate);
