#include "audio_core/AlertMixer.h"
#include "esp_heap_caps.h"
#include <algorithm>

std::shared_ptr<AlertClip> AlertClip::create(size_t samples) {
    if (samples == 0) return nullptr;
    auto* pcm = static_cast<int16_t*>(
        heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pcm) return nullptr;
    return std::shared_ptr<AlertClip>(new AlertClip(pcm, samples));
}

AlertClip::~AlertClip() {
    heap_caps_free(m_pcm);
}

void AlertClip::fadeEdges(size_t in_samples, size_t out_samples) {
    in_samples = std::min(in_samples, m_samples);
    out_samples = std::min(out_samples, m_samples);
    for (size_t i = 0; i < in_samples; ++i) {
        m_pcm[i] = static_cast<int16_t>(static_cast<int32_t>(m_pcm[i]) * static_cast<int32_t>(i) /
                                        static_cast<int32_t>(in_samples));
    }
    for (size_t i = 0; i < out_samples; ++i) {
        int16_t& s = m_pcm[m_samples - 1 - i];
        s = static_cast<int16_t>(static_cast<int32_t>(s) * static_cast<int32_t>(i) /
                                 static_cast<int32_t>(out_samples));
    }
}

void AlertMixer::setClip(size_t slot, AlertClipPtr clip) {
    if (slot >= SLOTS) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_slots[slot].clip = std::move(clip);
}

AlertClipPtr AlertMixer::clip(size_t slot) const {
    if (slot >= SLOTS) return nullptr;
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_slots[slot].clip;
}

void AlertMixer::setGain(size_t slot, float gain) {
    if (slot >= SLOTS) return;
    gain = std::clamp(gain, 0.0f, 4.0f);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_slots[slot].gain_q12 = static_cast<int32_t>(gain * 4096.0f + 0.5f);
}

float AlertMixer::gain(size_t slot) const {
    if (slot >= SLOTS) return 0.0f;
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<float>(m_slots[slot].gain_q12) / 4096.0f;
}

void AlertMixer::setEnabled(size_t slot, bool enabled) {
    if (slot >= SLOTS) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_slots[slot].enabled = enabled;
}

bool AlertMixer::enabled(size_t slot) const {
    if (slot >= SLOTS) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_slots[slot].enabled;
}

bool AlertMixer::play(size_t slot, bool force, bool loop) {
    if (slot >= SLOTS) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    const Slot& s = m_slots[slot];
    if ((!s.enabled && !force) || !s.clip || s.clip->size() == 0) return false;
    m_pending = static_cast<int>(slot);
    m_pending_loop = loop;
    m_stop_requested = false;
    return true;
}

void AlertMixer::stop() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending = NONE;
    m_stop_requested = true;
}

bool AlertMixer::active() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_clip != nullptr || m_pending != NONE;
}

int AlertMixer::current() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pending != NONE ? m_pending : m_slot;
}

// Called with the lock held, when nothing is playing.
void AlertMixer::beginPending() {
    const Slot& s = m_slots[m_pending];
    m_clip = s.clip;
    m_gain_q12 = s.gain_q12;
    m_slot = m_pending;
    m_loop = m_pending_loop;
    m_pos = 0;
    m_fade_left = 0;
    m_pending = NONE;
}

size_t AlertMixer::render(int16_t* out, size_t n, Event& event) {
    event = Event::None;
    size_t written = 0;
    // Samples dropped from the clip: the returned shared_ptr is released after
    // the lock, so a replaced clip's PSRAM is never freed while holding it.
    AlertClipPtr finished;
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_clip && m_fade_left == 0 && (m_pending != NONE || m_stop_requested)) {
            m_fade_left = std::min(FADE_SAMPLES, m_clip->size() - m_pos);
        }
        m_stop_requested = false;
        if (!m_clip && m_pending != NONE) beginPending();

        while (written < n && m_clip) {
            const int16_t* pcm = m_clip->data();
            size_t left = m_clip->size() - m_pos;
            const bool fading = m_fade_left > 0;
            if (fading) left = std::min(left, m_fade_left);
            const size_t count = std::min(left, n - written);

            for (size_t i = 0; i < count; ++i) {
                int32_t v = (static_cast<int32_t>(pcm[m_pos + i]) * m_gain_q12) >> 12;
                if (fading) {
                    const size_t remain = m_fade_left - i;
                    v = v * static_cast<int32_t>(remain) / static_cast<int32_t>(FADE_SAMPLES);
                }
                out[written + i] = static_cast<int16_t>(std::clamp<int32_t>(v, -32768, 32767));
            }
            m_pos += count;
            written += count;
            if (fading) m_fade_left -= count;

            if (m_pos >= m_clip->size() && m_loop && !fading) {
                m_pos = 0;
                continue;
            }
            if (m_pos >= m_clip->size() || (fading && m_fade_left == 0)) {
                finished = std::move(m_clip);
                m_clip = nullptr;
                m_slot = NONE;
                m_fade_left = 0;
                if (m_pending != NONE) beginPending();
            }
        }

        const bool now_active = m_clip != nullptr || m_pending != NONE;
        if (now_active && !m_was_active) event = Event::Started;
        else if (!now_active && m_was_active) event = Event::Ended;
        // A clip that starts and ends within one call still reports Started;
        // Ended follows on the next call.
        if (!now_active && !m_was_active && written > 0) {
            event = Event::Started;
            m_was_active = true;
        } else {
            m_was_active = now_active;
        }
    }
    return written;
}
