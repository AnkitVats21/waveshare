#pragma once

#include "audio_core/AlertMixer.h"
#include <cstdint>

enum AlertType : uint8_t {
    ALERT_WAKE_CONFIRM,
    ALERT_READY_TO_SPEAK,
    ALERT_SESSION_END,
    ALERT_ERROR,
    ALERT_OFFLINE,
    ALERT_REMINDER,
    ALERT_COUNT
};

// "wake_confirm", ...; also the default file name under /sdcard/media/alert/.
const char* alertName(AlertType type);
// ALERT_COUNT if the name is unknown.
AlertType alertFromName(const char* name);

/**
 * @brief Alert chimes, held as decoded PCM in PSRAM and mixed by the speaker task.
 *
 * begin() renders the built-in tones, so every alert can sound from boot. A
 * loader (AlertLibrary) later decodes the files on the SD card and swaps them
 * in with setClip(). Playback never touches the SD card: the speaker task
 * pulls samples straight from the clip through render().
 */
class AlertPlayer {
public:
    static AlertPlayer& getInstance();

    bool begin();

    /**
     * @brief Plays an alert, replacing any that is playing.
     *        Thread-safe and non-blocking.
     */
    void playAlert(AlertType type);
    // Plays the alert even if it is disabled (dashboard preview).
    void preview(AlertType type);
    void stop();

    // Built-in alarm tone, looped until stopAlarmTone(). It uses its own mixer
    // slot (not an AlertType: it is not configurable from the Sounds card) and
    // never touches the SD card, so it is the fallback when an alarm song fails.
    // name picks a pattern (alarmToneName); unknown or null is "classic".
    void startAlarmTone(const char* name = nullptr);
    void stopAlarmTone();
    // The built-in alarm patterns: index 0 is the default ("classic").
    static size_t alarmToneCount();
    static const char* alarmToneName(size_t i);
    // Index of the named pattern, or -1.
    static int alarmToneIndex(const char* name);
    // While an alarm owns the device, alerts and previews are ignored so no
    // chime plays over it (or over its snooze silence).
    void setAlarmActive(bool active);
    bool alarmActive() const { return m_alarm_active; }

    // Speaker task: writes up to n samples of the alert track, returns the count.
    size_t render(int16_t* out, size_t n);

    void setClip(AlertType type, AlertClipPtr clip);
    AlertClipPtr clip(AlertType type) const;
    // The built-in tone for the alert, rendered at begin().
    AlertClipPtr builtin(AlertType type) const;
    void setGainDb(AlertType type, float db);
    void setEnabled(AlertType type, bool enabled);
    // Length of the alert's clip in ms, 0 if none.
    uint32_t clipMs(AlertType type) const;

private:
    AlertPlayer() = default;
    AlertPlayer(const AlertPlayer&) = delete;
    AlertPlayer& operator=(const AlertPlayer&) = delete;

    AlertMixer m_mixer;
    AlertClipPtr m_builtin[ALERT_COUNT];
    static constexpr size_t ALARM_SLOT = ALERT_COUNT;
    int m_alarm_pattern = 0;   // pattern in ALARM_SLOT
    static_assert(ALARM_SLOT < AlertMixer::SLOTS, "one mixer slot per alert, plus the alarm");
    volatile bool m_alarm_active = false;
    // esp_timer time of the last playAlert(), for the trigger-to-mix latency log
    volatile int64_t m_requested_us = 0;

    static constexpr const char* TAG = "AlertPlayer";
};
