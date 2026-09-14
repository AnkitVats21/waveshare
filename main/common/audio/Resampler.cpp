#include "Resampler.h"

void LinearResampler::resample(const int16_t* src, size_t srcFrames,
                                int16_t* dst, size_t dstFrames, int channels) const {
    if (srcFrames == 0 || dstFrames == 0) return;
    float ratio = static_cast<float>(srcFrames) / static_cast<float>(dstFrames);
    for (size_t i = 0; i < dstFrames; ++i) {
        float src_pos = i * ratio;
        size_t idx = static_cast<size_t>(src_pos);
        float frac = src_pos - idx;
        size_t next_idx = (idx + 1 < srcFrames) ? (idx + 1) : idx;
        for (int ch = 0; ch < channels; ++ch) {
            float s0 = src[idx * channels + ch];
            float s1 = src[next_idx * channels + ch];
            dst[i * channels + ch] = static_cast<int16_t>(s0 + frac * (s1 - s0));
        }
    }
}

size_t computeResampledFrames(size_t srcFrames, uint32_t srcRate, uint32_t dstRate) {
    if (srcRate == 0) return 0;
    return static_cast<size_t>((static_cast<uint64_t>(srcFrames) * dstRate) / srcRate);
}
