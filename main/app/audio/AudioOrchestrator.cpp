#include "AudioOrchestrator.h"
#include "app/audio/AudioPipelineManager.h"
#include "app/audio/SpeakerPlayback.h"
#include "esp_log.h"
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
    ESP_LOGI(TAG, "notifyWakeWordDetected: ducking background media immediately");
    if (m_media_active) {
        duckMedia(0.20f, 50);
        m_media_ducked = true;
    }
}

void AudioOrchestrator::notifyVoiceStarted() {
    ESP_LOGI(TAG, "notifyVoiceStarted: Assistant voice starting");
    m_voice_active = true;
    if (m_media_active) {
        m_media_paused_by_voice = true;
        broadcastFocusEvent(AudioTrack::MEDIA, FocusEvent::LOSS_PAUSE);
    }
}

void AudioOrchestrator::notifyVoiceEnded() {
    ESP_LOGI(TAG, "notifyVoiceEnded: Assistant voice finished");
    m_voice_active = false;
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
    if (m_media_active && !m_voice_active) {
        duckMedia(0.20f, 50);
        m_media_ducked = true;
    }
}

void AudioOrchestrator::notifyAlertEnded() {
    ESP_LOGI(TAG, "notifyAlertEnded: Alert chime finished");
    m_alert_active = false;
    if (m_media_ducked && !m_voice_active && !m_media_paused_by_voice) {
        unduckMedia(100);
        m_media_ducked = false;
    }
}

void AudioOrchestrator::notifyAlarmStarted() {
    ESP_LOGI(TAG, "notifyAlarmStarted: Alarm ringing");
    m_alarm_active = true;
    if (m_media_active) {
        broadcastFocusEvent(AudioTrack::MEDIA, FocusEvent::LOSS_PAUSE);
    }
}

void AudioOrchestrator::notifyAlarmEnded() {
    ESP_LOGI(TAG, "notifyAlarmEnded: Alarm stopped");
    m_alarm_active = false;
    if (!m_voice_active) {
        broadcastFocusEvent(AudioTrack::MEDIA, FocusEvent::GAIN);
        unduckMedia(100);
    }
}

void AudioOrchestrator::notifyMediaStarted() {
    ESP_LOGI(TAG, "notifyMediaStarted: Media playback active");
    m_media_active = true;
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
}

void AudioOrchestrator::duckMedia(float targetGain, uint32_t rampMs) {
    SpeakerPlaybackTask* spk = AudioPipelineManager::getSpeakerTask();
    if (spk) {
        spk->setMediaGain(targetGain, rampMs);
    }
}

void AudioOrchestrator::unduckMedia(uint32_t rampMs) {
    SpeakerPlaybackTask* spk = AudioPipelineManager::getSpeakerTask();
    if (spk) {
        spk->setMediaGain(1.0f, rampMs);
    }
}
