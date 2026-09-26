#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @brief Streaming rational-ratio resampler with a windowed-sinc low-pass,
 * for recordings (e.g. 32 kHz mic feed -> 24 kHz for Opus).
 *
 * Unlike LinearResampler it keeps history across calls, so chunk boundaries
 * are seamless, and it filters out content above the new Nyquist instead of
 * aliasing it. The ratio out/in reduces to L/M (32k->24k is 3/4), so only L
 * coefficient sets are needed. Buffers live in PSRAM.
 */
class PolyphaseResampler {
public:
    ~PolyphaseResampler();

    bool init(uint32_t in_rate, uint32_t out_rate, int channels);

    /** Upper bound on output frames for `in_frames` of input. */
    size_t maxOutput(size_t in_frames) const { return in_frames * m_l / m_m + 2; }

    /** Resamples interleaved PCM; returns frames written to `out`
     *  (at most maxOutput(in_frames)). */
    size_t process(const int16_t* in, size_t in_frames, int16_t* out);

private:
    static constexpr int kTaps = 32;

    int      m_channels = 1;
    uint32_t m_l = 1;           // output step (numerator of out/in)
    uint32_t m_m = 1;           // input step (denominator of out/in)
    int16_t* m_coeffs = nullptr;   // m_l rows of kTaps, Q14 (each row sums to 1 << 14)
    int16_t* m_work = nullptr;     // kTaps history frames + current input
    size_t   m_work_cap = 0;       // frames
    // Next output position in the work buffer: frame index plus phase/m_l.
    // Kept incrementally: a 64-bit divide per sample is slow on Xtensa.
    size_t   m_pos = 0;
    uint32_t m_phase = 0;
};
