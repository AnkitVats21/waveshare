#pragma once

#include <cmath>

// Treble control for the speaker: one high-shelf biquad (RBJ cookbook,
// shelf slope 1) that lowers everything above CORNER_HZ by `db`, leaving the
// lows and mids alone. 0 dB is a bypass (no work, no state). Portable;
// host-tested (test_treble_filter.py).
class TrebleFilter {
public:
    static constexpr int MIN_DB = -12;
    static constexpr int MAX_DB = 0;
    static constexpr float CORNER_HZ = 3000.0f;

    // Recomputes the coefficients; keeps the filter's history so a change
    // while playing doesn't click.
    void set(int db, float sample_rate) {
        if (db < MIN_DB) db = MIN_DB;
        if (db > MAX_DB) db = MAX_DB;
        m_db = db;
        if (db == 0) {
            reset();
            return;
        }
        const float A = std::pow(10.0f, db / 40.0f);
        const float w0 = 2.0f * static_cast<float>(M_PI) * CORNER_HZ / sample_rate;
        const float c = std::cos(w0);
        const float alpha = std::sin(w0) / 2.0f * std::sqrt(2.0f);
        const float k = 2.0f * std::sqrt(A) * alpha;
        const float a0 = (A + 1) - (A - 1) * c + k;
        m_b0 = A * ((A + 1) + (A - 1) * c + k) / a0;
        m_b1 = -2 * A * ((A - 1) + (A + 1) * c) / a0;
        m_b2 = A * ((A + 1) + (A - 1) * c - k) / a0;
        m_a1 = 2 * ((A - 1) - (A + 1) * c) / a0;
        m_a2 = ((A + 1) - (A - 1) * c - k) / a0;
    }

    int db() const { return m_db; }
    bool active() const { return m_db != 0; }
    void reset() { m_z1 = m_z2 = 0.0f; }

    // One sample (transposed direct form II).
    float process(float x) {
        if (m_db == 0) return x;
        const float y = m_b0 * x + m_z1;
        m_z1 = m_b1 * x - m_a1 * y + m_z2;
        m_z2 = m_b2 * x - m_a2 * y;
        return y;
    }

private:
    int m_db = 0;
    float m_b0 = 1, m_b1 = 0, m_b2 = 0, m_a1 = 0, m_a2 = 0;
    float m_z1 = 0, m_z2 = 0;
};
