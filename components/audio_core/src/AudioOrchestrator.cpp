#include "audio_core/AudioOrchestrator.h"
#include "audio_core/SpeakerPlayback.h"
#include "esp_log.h"
#include "core_sysdb/EmbeddedSysDb.h"
#include <algorithm>

AudioOrchestrator& AudioOrchestrator::getInstance() {
    static AudioOrchestrator instance;
    return instance;
}

AudioOrchestrator::AudioOrchestrator() {
    m_mutex = xSemaphoreCreateMutex();
}

AudioOrchestrator::~AudioOrchestrator() {
    if (m_mutex) {
        vSemaphoreDelete(m_mutex);
        m_mutex = nullptr;
    }
}

bool AudioOrchestrator::begin() {
    ESP_LOGI(TAG, "AudioOrchestrator initialized.");
    return true;
}

void AudioOrchestrator::addObserver(IAudioFocusObserver* observer) {
    if (!observer) return;
    if (xSemaphoreTake(m_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (std::find(m_observers.begin(), m_observers.end(), observer) == m_observers.end()) {
            m_observers.push_back(observer);
        }
        xSemaphoreGive(m_mutex);
    }
}

void AudioOrchestrator::removeObserver(IAudioFocusObserver* observer) {
    if (!observer) return;
    if (xSemaphoreTake(m_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        m_observers.erase(std::remove(m_observers.begin(), m_observers.end(), observer), m_observers.end());
        xSemaphoreGive(m_mutex);
    }
}

void AudioOrchestrator::broadcastFocusEvent(AudioTrack track, FocusEvent event) {
    std::vector<IAudioFocusObserver*> copy;
    if (xSemaphoreTake(m_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        copy = m_observers;
        xSemaphoreGive(m_mutex);
    }
    for (auto* obs : copy) {
        if (obs) {
            obs->onAudioFocusChange(track, event);
        }
    }
}

void AudioOrchestrator::notifyWakeWordDetected() {
    // Pause (not duck) for the whole assistant session; NexusPlayer resumes it when
    // the session returns to Idle. Pausing also clears m_media_active (via
    // notifyMediaStopped), so the per-turn voice pause/resume below stays out of it.
    if (m_under_voice) {
        ESP_LOGI(TAG, "notifyWakeWordDetected: music under the voice keeps playing");
        return;
    }
    ESP_LOGI(TAG, "notifyWakeWordDetected: pausing background media for the session");
    if (m_media_active) {
        broadcastFocusEvent(AudioTrack::MEDIA, FocusEvent::LOSS_PAUSE);
    }
}

void AudioOrchestrator::notifyVoiceStarted() {
    ESP_LOGI(TAG, "notifyVoiceStarted: Assistant voice starting");
    m_voice_active = true;
    if (m_under_voice) {
        if (m_media_active && !m_media_ducked) {
            duckMedia(m_under_voice_gain, UNDER_VOICE_DUCK_MS);
            if (m_speaker_task) m_speaker_task->holdVoice(UNDER_VOICE_DUCK_MS);
            m_media_ducked = true;
        }
    } else if (m_media_active) {
        m_media_paused_by_voice = true;
        broadcastFocusEvent(AudioTrack::MEDIA, FocusEvent::LOSS_PAUSE);
    }
}

void AudioOrchestrator::notifyVoiceEnded() {
    ESP_LOGI(TAG, "notifyVoiceEnded: Assistant voice finished");
    m_voice_active = false;
    if (m_under_voice) {
        return;   // stays ducked; the owner brings it back
    }
    if (m_media_paused_by_voice) {
        m_media_paused_by_voice = false;
        broadcastFocusEvent(AudioTrack::MEDIA, FocusEvent::GAIN);
        unduckMedia(100);
        m_media_ducked = false;
    }
}

void AudioOrchestrator::notifyAlertStarted() {
    ESP_LOGI(TAG, "notifyAlertStarted: Alert chime starting");
    m_alert_active = true;
    if (m_under_voice) return;   // the music's level is its owner's
    // The alarm's built-in tone plays on the alert track; it must not duck the
    // alarm (nor would any other chime play while an alarm rings).
    if (m_media_active && !m_voice_active && !m_alarm_active) {
        duckMedia(0.20f, 50);
        m_media_ducked = true;
    }
}

void AudioOrchestrator::notifyAlertEnded() {
    ESP_LOGI(TAG, "notifyAlertEnded: Alert chime finished");
    m_alert_active = false;
    if (m_under_voice) return;
    if (m_media_ducked && !m_voice_active && !m_media_paused_by_voice) {
        unduckMedia(100);
        m_media_ducked = false;
    }
}

// The alarm is the top priority. It does not pause and resume media through
// focus events: AlarmService takes NexusPlayer over (NexusPlayer::beginAlarm)
// and gives it back, restoring the music itself.
void AudioOrchestrator::notifyAlarmStarted() {
    ESP_LOGI(TAG, "notifyAlarmStarted: Alarm ringing");
    m_alarm_active = true;
    m_media_paused_by_voice = false;
}

void AudioOrchestrator::notifyAlarmEnded() {
    ESP_LOGI(TAG, "notifyAlarmEnded: Alarm stopped");
    m_alarm_active = false;
    unduckMedia(0);
    m_media_ducked = false;
}

void AudioOrchestrator::notifyMediaStarted() {
    ESP_LOGI(TAG, "notifyMediaStarted: Media playback active");
    m_media_active = true;
    if (m_alarm_active) {
        // The alarm song: AlarmService sets the media gain for its fade-in.
        return;
    }
    if (m_under_voice) {
        // Started (or looped) under a reply: keep its level.
        if (m_voice_active && !m_media_ducked) {
            duckMedia(m_under_voice_gain, 0);
            m_media_ducked = true;
        }
        return;
    }
    if (m_voice_active) {
        // Voice is running; immediately pause media
        m_media_paused_by_voice = true;
        broadcastFocusEvent(AudioTrack::MEDIA, FocusEvent::LOSS_PAUSE);
    } else if (m_alert_active) {
        // Alert is active; start ducked
        duckMedia(0.20f, 0);
        m_media_ducked = true;
    } else {
        unduckMedia(0);
        m_media_ducked = false;
    }
}

void AudioOrchestrator::notifyMediaStopped() {
    ESP_LOGI(TAG, "notifyMediaStopped: Media playback idle");
    m_media_active = false;
    m_media_paused_by_voice = false;
    m_media_ducked = false;
    EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
        s.media.is_ducked = false;
    });
}

void AudioOrchestrator::setMusicUnderVoice(bool on, float gain) {
    ESP_LOGI(TAG, "Music under the voice %s (duck to %d%%)", on ? "on" : "off", (int)(gain * 100 + 0.5f));
    m_under_voice_gain = gain;
    m_under_voice = on;
    if (!on) m_media_ducked = false;   // the owner sets the level from here
}

void AudioOrchestrator::duckMedia(float targetGain, uint32_t rampMs) {
    if (m_speaker_task) {
        m_speaker_task->setMediaGain(targetGain, rampMs);
    }
    EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
        s.media.is_ducked = true;
    });
}

void AudioOrchestrator::unduckMedia(uint32_t rampMs) {
    if (m_speaker_task) {
        m_speaker_task->setMediaGain(1.0f, rampMs);
    }
    EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
        s.media.is_ducked = false;
    });
}

void AudioOrchestrator::fadeInMedia(float from, uint32_t rampMs) {
    if (m_speaker_task) {
        m_speaker_task->setMediaGain(from, 0);
        m_speaker_task->setMediaGain(1.0f, rampMs);
    }
}
