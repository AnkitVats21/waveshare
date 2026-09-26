#include "audio_core/AlertPlayer.h"
#include "audio_core/AlertTones.h"
#include "audio_core/AudioOrchestrator.h"
#include "core_sysdb/AudioRates.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <cmath>
#include <cstring>

namespace {

const char* const NAMES[ALERT_COUNT] = {
    "wake_confirm", "ready_to_speak", "session_end", "error", "offline",
};

const ToneNote WAKE_CONFIRM[] = {
    {784.0f, 8000, 80, 15},    // G5
    {987.8f, 8000, 80, 15},    // B5
};
const ToneNote READY_TO_SPEAK[] = {
    {523.3f, 10000, 90, 15},   // C5
    {659.3f, 10000, 90, 15},   // E5
    {784.0f, 10000, 130, 20},  // G5
};
const ToneNote SESSION_END[] = {
    {659.3f, 7000, 100, 20},   // E5
    {523.3f, 7000, 120, 20},   // C5
};
const ToneNote ERROR_BEEPS[] = {
    {440.0f, 9000, 60, 10},
    {0.0f, 0, 40, 0},
    {440.0f, 9000, 60, 10},
};
const ToneNote OFFLINE[] = {
    {146.8f, 8000, 120, 25},   // D3
};

// Classic alarm clock: four beeps, then a pause. Looped while ringing.
const ToneNote ALARM_BEEPS[] = {
    {1046.5f, 14000, 100, 8}, {0.0f, 0, 60, 0},   // C6
    {1046.5f, 14000, 100, 8}, {0.0f, 0, 60, 0},
    {1046.5f, 14000, 100, 8}, {0.0f, 0, 60, 0},
    {1046.5f, 14000, 100, 8}, {0.0f, 0, 520, 0},
};

struct ToneSet { const ToneNote* notes; size_t count; };
template <size_t N> constexpr ToneSet tones(const ToneNote (&n)[N]) { return {n, N}; }
const ToneSet TONES[ALERT_COUNT] = {
    tones(WAKE_CONFIRM), tones(READY_TO_SPEAK), tones(SESSION_END), tones(ERROR_BEEPS), tones(OFFLINE),
};

} // namespace

const char* alertName(AlertType type) {
    return type < ALERT_COUNT ? NAMES[type] : "unknown";
}

AlertType alertFromName(const char* name) {
    for (int i = 0; i < ALERT_COUNT; ++i) {
        if (name && strcmp(name, NAMES[i]) == 0) return static_cast<AlertType>(i);
    }
    return ALERT_COUNT;
}

AlertPlayer& AlertPlayer::getInstance() {
    static AlertPlayer instance;
    return instance;
}

bool AlertPlayer::begin() {
    size_t bytes = 0;
    for (int i = 0; i < ALERT_COUNT; ++i) {
        std::shared_ptr<AlertClip> clip = synthesizeTones(TONES[i].notes, TONES[i].count, MIXER_SAMPLE_RATE);
        if (!clip) {
            ESP_LOGE(TAG, "No PSRAM for the built-in %s tone", NAMES[i]);
            continue;
        }
        bytes += clip->size() * sizeof(int16_t);
        m_builtin[i] = clip;
        m_mixer.setClip(i, clip);
    }
    std::shared_ptr<AlertClip> alarm = synthesizeTones(ALARM_BEEPS, sizeof(ALARM_BEEPS) / sizeof(ALARM_BEEPS[0]),
                                                       MIXER_SAMPLE_RATE);
    if (alarm) {
        bytes += alarm->size() * sizeof(int16_t);
        m_mixer.setClip(ALARM_SLOT, alarm);
    } else {
        ESP_LOGE(TAG, "No PSRAM for the built-in alarm tone");
    }
    ESP_LOGI(TAG, "Built-in tones ready (%u bytes)", (unsigned)bytes);
    return true;
}

void AlertPlayer::playAlert(AlertType type) {
    if (type >= ALERT_COUNT || m_alarm_active) return;
    m_requested_us = esp_timer_get_time();
    if (!m_mixer.play(type)) {
        ESP_LOGD(TAG, "%s is disabled or has no clip", NAMES[type]);
    }
}

void AlertPlayer::preview(AlertType type) {
    if (type >= ALERT_COUNT || m_alarm_active) return;
    m_requested_us = esp_timer_get_time();
    m_mixer.play(type, true);
}

uint32_t AlertPlayer::clipMs(AlertType type) const {
    AlertClipPtr c = clip(type);
    return c ? (uint32_t)((uint64_t)c->size() * 1000 / MIXER_SAMPLE_RATE) : 0;
}

void AlertPlayer::stop() {
    m_mixer.stop();
}

void AlertPlayer::startAlarmTone() {
    m_requested_us = esp_timer_get_time();
    if (!m_mixer.play(ALARM_SLOT, true, true)) ESP_LOGE(TAG, "No built-in alarm tone");
}

void AlertPlayer::stopAlarmTone() {
    if (m_mixer.current() == static_cast<int>(ALARM_SLOT)) m_mixer.stop();
}

void AlertPlayer::setAlarmActive(bool active) {
    m_alarm_active = active;
    // A chime already playing is cut so the alarm starts clean.
    if (active && m_mixer.current() != static_cast<int>(ALARM_SLOT)) m_mixer.stop();
}

size_t AlertPlayer::render(int16_t* out, size_t n) {
    AlertMixer::Event event;
    size_t written = m_mixer.render(out, n, event);
    if (event == AlertMixer::Event::Started) {
        ESP_LOGI(TAG, "alert started %lld us after request",
                 (long long)(esp_timer_get_time() - m_requested_us));
        AudioOrchestrator::getInstance().notifyAlertStarted();
    } else if (event == AlertMixer::Event::Ended) {
        AudioOrchestrator::getInstance().notifyAlertEnded();
    }
    return written;
}

void AlertPlayer::setClip(AlertType type, AlertClipPtr clip) {
    if (type < ALERT_COUNT) m_mixer.setClip(type, std::move(clip));
}

AlertClipPtr AlertPlayer::clip(AlertType type) const {
    return type < ALERT_COUNT ? m_mixer.clip(type) : nullptr;
}

AlertClipPtr AlertPlayer::builtin(AlertType type) const {
    return type < ALERT_COUNT ? m_builtin[type] : nullptr;
}

void AlertPlayer::setGainDb(AlertType type, float db) {
    if (type < ALERT_COUNT) m_mixer.setGain(type, powf(10.0f, db / 20.0f));
}

void AlertPlayer::setEnabled(AlertType type, bool enabled) {
    if (type < ALERT_COUNT) m_mixer.setEnabled(type, enabled);
}
