#include "app/audio/recording/PolyphaseResampler.h"

#include <cmath>
#include <cstring>
#include <numeric>
#include "esp_heap_caps.h"

namespace {
void* psramAlloc(size_t bytes) {
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
} // namespace

PolyphaseResampler::~PolyphaseResampler() {
    heap_caps_free(m_coeffs);
    heap_caps_free(m_work);
}

bool PolyphaseResampler::init(uint32_t in_rate, uint32_t out_rate, int channels) {
    if (in_rate == 0 || out_rate == 0 || channels <= 0) return false;
    uint32_t g = std::gcd(in_rate, out_rate);
    m_l = out_rate / g;
    m_m = in_rate / g;
    m_channels = channels;
    if (m_l > 64) return false;  // keeps the coefficient table small

    m_coeffs = static_cast<int16_t*>(psramAlloc(sizeof(int16_t) * m_l * kTaps));
    if (!m_coeffs) return false;

    // Cut off a little below the lower Nyquist.
    const double cutoff = 0.45 * std::fmin(in_rate, out_rate) / in_rate;  // cycles per input sample
    const int half = kTaps / 2;
    for (uint32_t phase = 0; phase < m_l; ++phase) {
        double taps[kTaps];
        double sum = 0;
        for (int k = 0; k < kTaps; ++k) {
            // Distance from the tap to the output position (in input samples).
            double d = (k - half + 1) - static_cast<double>(phase) / m_l;
            double x = 2.0 * cutoff * d;
            double sinc = (std::fabs(x) < 1e-9) ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
            double w = 0.54 + 0.46 * std::cos(M_PI * d / (half + 1));  // Hamming
            taps[k] = sinc * w;
            sum += taps[k];
        }
        // Integer taps: the inner loop then needs no float or rounding calls.
        int16_t* row = m_coeffs + phase * kTaps;
        for (int k = 0; k < kTaps; ++k) row[k] = static_cast<int16_t>(std::lround(taps[k] / sum * (1 << 14)));
    }

    // History starts as silence; the first real input frame is at index kTaps.
    m_pos = kTaps;
    m_phase = 0;
    return true;
}

size_t PolyphaseResampler::process(const int16_t* in, size_t in_frames, int16_t* out) {
    if (!m_coeffs || in_frames == 0) return 0;
    const size_t frame_bytes = sizeof(int16_t) * m_channels;

    size_t need = kTaps + in_frames;
    if (need > m_work_cap) {
        auto* grown = static_cast<int16_t*>(psramAlloc(need * frame_bytes));
        if (!grown) return 0;
        if (m_work) {
            std::memcpy(grown, m_work, kTaps * frame_bytes);
            heap_caps_free(m_work);
        } else {
            std::memset(grown, 0, kTaps * frame_bytes);
        }
        m_work = grown;
        m_work_cap = need;
    }
    std::memcpy(m_work + kTaps * m_channels, in, in_frames * frame_bytes);

    const int half = kTaps / 2;
    size_t produced = 0;
    while (m_pos + half < need) {
        const int16_t* row = m_coeffs + m_phase * kTaps;
        const int16_t* src = m_work + (m_pos + 1 - half) * m_channels;
        for (int ch = 0; ch < m_channels; ++ch) {
            int32_t acc = 1 << 13;  // rounding
            for (int k = 0; k < kTaps; ++k) acc += row[k] * src[k * m_channels + ch];
            acc >>= 14;
            out[produced * m_channels + ch] = static_cast<int16_t>(acc > 32767 ? 32767 : (acc < -32768 ? -32768 : acc));
        }
        ++produced;
        m_phase += m_m;
        m_pos += m_phase / m_l;
        m_phase %= m_l;
    }

    // Keep the last kTaps frames as history for the next call.
    std::memmove(m_work, m_work + in_frames * m_channels, kTaps * frame_bytes);
    m_pos -= in_frames;
    return produced;
}
