#include "audio_core/AlertTones.h"
#include <cmath>

std::shared_ptr<AlertClip> synthesizeTones(const ToneNote* notes, size_t count, uint32_t sample_rate) {
    size_t total = 0;
    for (size_t n = 0; n < count; ++n) total += (size_t)sample_rate * notes[n].ms / 1000;
    std::shared_ptr<AlertClip> clip = AlertClip::create(total);
    if (!clip) return nullptr;

    int16_t* out = clip->data();
    for (size_t n = 0; n < count; ++n) {
        const ToneNote& note = notes[n];
        const size_t len = (size_t)sample_rate * note.ms / 1000;
        const size_t fade = (size_t)sample_rate * note.fade_ms / 1000;
        const float step = 2.0f * 3.14159265f * note.hz / (float)sample_rate;
        for (size_t i = 0; i < len; ++i) {
            float env = 1.0f;
            if (fade > 0 && i < fade) env = (float)i / (float)fade;
            else if (fade > 0 && i >= len - fade) env = (float)(len - i) / (float)fade;
            out[i] = note.hz > 0.0f ? (int16_t)(env * (float)note.amplitude * sinf(step * (float)i)) : 0;
        }
        out += len;
    }
    return clip;
}
