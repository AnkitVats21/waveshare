#pragma once

#include <algorithm>
#include <cmath>

// Bass and treble for the speaker: two shelf biquads (RBJ cookbook, shelf
// slope 1). Bass moves everything below BASS_HZ, treble everything above
// TREBLE_HZ, by MIN_DB..MAX_DB. 0 dB is a bypass (no work, no state). A
// boost lowers the whole mix by the largest boost first (headroom), so
// music mastered near full scale doesn't clip. Portable; host-tested
// (test_tone_control.py).
class ShelfFilter {
public:
    enum class Kind { Low, High };

    // Recomputes the coefficients; keeps the history so a change while
    // playing doesn't click.
    void set(Kind kind, float corner_hz, int db, float sample_rate) {
        m_db = db;
        if (db == 0) {
            reset();
            return;
        }
        const float A = std::pow(10.0f, db / 40.0f);
        const float w0 = 2.0f * static_cast<float>(M_PI) * corner_hz / sample_rate;
        const float c = std::cos(w0);
        const float k = 2.0f * std::sqrt(A) * std::sin(w0) / 2.0f * std::sqrt(2.0f);
        // The low shelf is the high shelf with the sign of (A - 1) * cos flipped.
        const float s = kind == Kind::High ? 1.0f : -1.0f;
        const float a0 = (A + 1) - s * (A - 1) * c + k;
        m_b0 = A * ((A + 1) + s * (A - 1) * c + k) / a0;
        m_b1 = -2 * s * A * ((A - 1) + s * (A + 1) * c) / a0;
        m_b2 = A * ((A + 1) + s * (A - 1) * c - k) / a0;
        m_a1 = 2 * s * ((A - 1) - s * (A + 1) * c) / a0;
        m_a2 = ((A + 1) - s * (A - 1) * c - k) / a0;
    }

    int db() const { return m_db; }
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

class ToneControl {
public:
    static constexpr int MIN_DB = -12;
    static constexpr int MAX_DB = 6;
    static constexpr float BASS_HZ = 250.0f;
    static constexpr float TREBLE_HZ = 3000.0f;

    static int clampDb(int db) { return std::clamp(db, MIN_DB, MAX_DB); }

    void set(int bass_db, int treble_db, float sample_rate) {
        bass_db = clampDb(bass_db);
        treble_db = clampDb(treble_db);
        m_bass.set(ShelfFilter::Kind::Low, BASS_HZ, bass_db, sample_rate);
        m_treble.set(ShelfFilter::Kind::High, TREBLE_HZ, treble_db, sample_rate);
        m_headroom = std::pow(10.0f, -std::max({0, bass_db, treble_db}) / 20.0f);
    }

    int bassDb() const { return m_bass.db(); }
    int trebleDb() const { return m_treble.db(); }
    bool active() const { return m_bass.db() != 0 || m_treble.db() != 0; }
    void reset() {
        m_bass.reset();
        m_treble.reset();
    }

    float process(float x) { return m_treble.process(m_bass.process(x * m_headroom)); }

private:
    ShelfFilter m_bass;
    ShelfFilter m_treble;
    float m_headroom = 1.0f;
};
