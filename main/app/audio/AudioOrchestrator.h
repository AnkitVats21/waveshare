#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <vector>
#include <cstdint>

enum class AudioTrack : uint8_t {
    VOICE,  // Gemini live assistant speech
    ALERT,  // Chimes, notifications, system sounds
    MEDIA,  // Music playback (NexusPlayer)
    ALARM   // Clock / timer alarms
};

enum class FocusEvent : uint8_t {
    GAIN,       // Focus granted / restored (e.g. resume playback)
    LOSS_DUCK,  // Duck playback volume (e.g. drop to 20%)
    LOSS_PAUSE, // Pause playback temporarily (e.g. voice active)
    LOSS_STOP   // Stop playback completely
};

class IAudioFocusObserver {
public:
    virtual ~IAudioFocusObserver() = default;
    virtual void onAudioFocusChange(AudioTrack track, FocusEvent event) = 0;
};

/**
 * @brief Central Audio Orchestrator and Focus Manager.
 *
 * Coordinates playback priorities across Voice, Alert, Media, and Alarm tracks.
 * Manages smooth software ducking/unducking and notifies observers (like NexusPlayer)
 * to pause or resume cleanly during assistant interactions.
 */
class AudioOrchestrator {
public:
    static AudioOrchestrator& getInstance();

    bool begin();

    // Observer management
    void addObserver(IAudioFocusObserver* observer);
    void removeObserver(IAudioFocusObserver* observer);

    // Lifecycle events
    void notifyWakeWordDetected();
    void notifyVoiceStarted();
    void notifyVoiceEnded();
    void notifyAlertStarted();
    void notifyAlertEnded();
    void notifyAlarmStarted();
    void notifyAlarmEnded();
    void notifyMediaStarted();
    void notifyMediaStopped();

    // Query states
    bool isVoiceActive() const { return m_voice_active; }
    bool isAlertActive() const { return m_alert_active; }
    bool isMediaActive() const { return m_media_active; }
    bool isAlarmActive() const { return m_alarm_active; }

    // Ducking controls (affects Media track)
    void duckMedia(float targetGain = 0.20f, uint32_t rampMs = 50);
    void unduckMedia(uint32_t rampMs = 100);

private:
    AudioOrchestrator();
    ~AudioOrchestrator();
    AudioOrchestrator(const AudioOrchestrator&) = delete;
    AudioOrchestrator& operator=(const AudioOrchestrator&) = delete;

    void broadcastFocusEvent(AudioTrack track, FocusEvent event);

    SemaphoreHandle_t m_mutex = nullptr;
    std::vector<IAudioFocusObserver*> m_observers;

    volatile bool m_voice_active = false;
    volatile bool m_alert_active = false;
    volatile bool m_media_active = false;
    volatile bool m_alarm_active = false;
    volatile bool m_media_paused_by_voice = false;
    volatile bool m_media_ducked = false;

    static constexpr const char* TAG = "AudioOrch";
};
