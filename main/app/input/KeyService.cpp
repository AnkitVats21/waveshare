#include "KeyService.h"
#include "esp_log.h"
#include "core_sysdb/EmbeddedSysDb.h"
#include "core_sysdb/thread_config.h"
#include "media_player/NexusPlayer.h"
#include "media_player/MusicPlaybackService.h"
#include "app/audio/recording/AudioRecorder.h"
#include "app/led/LedService.h"
#include "audio_core/AlertPlayer.h"
#include "hal/Board_defs.h"
#include "services/power/DeepSleep.h"
#include "freertos/idf_additions.h"
#include <algorithm>

static auto& sysdb = EmbeddedSysDb::getInstance();

static constexpr uint32_t POLL_MS = 20;
// The LEDs start filling this far into a KEY_2 hold.
static constexpr uint32_t SLEEP_FILL_FROM_MS = 1000;

void KeyService::key2Hold(uint32_t ticks) {
    using Services::DeepSleep::SLEEP_HOLD_MS;
    const uint32_t ms = ticks * POLL_MS;
    if (m_key2Sleep || ms < SLEEP_FILL_FROM_MS) return;
    if (ms >= SLEEP_HOLD_MS) {
        m_key2Sleep = true;
        LedService::setPowerHold(LED_STRIP_LED_COUNT);
        AlertPlayer::getInstance().playAlert(ALERT_SESSION_END);
        ESP_LOGI(TAG, "Key 2 held %u ms: deep sleep on release", (unsigned)SLEEP_HOLD_MS);
        return;
    }
    LedService::setPowerHold(1 + (int)((ms - SLEEP_FILL_FROM_MS) * (LED_STRIP_LED_COUNT - 1) /
                                       (SLEEP_HOLD_MS - SLEEP_FILL_FROM_MS)));
}

KeyService::KeyService(ExpanderKeyInput &input)
    : ReactorTask({
          "key_svc",
          ThreadConfig::StackSize::STACK_MEDIUM,
          ThreadConfig::Priority::KEY_POLL,
          ThreadConfig::CORE_NETWORK,
          0 // Pure writer, does not watch any state components
      })
    , m_input(input)
{}

bool KeyService::begin() {
    ESP_LOGI(TAG, "KeyService operational.");
    return true;
}

void KeyService::onStateChanged(ComponentMask changed, const SystemState& snap) {
    // Pure writer, nothing to react to
}

void KeyService::run() {
    using KeyId = ExpanderKeyInput::KeyId;

    const KeyId keys[5] = {
        KeyId::KEY_1, KeyId::KEY_2, KeyId::KEY_3, KeyId::KEY_4, KeyId::KEY_5,
    };

    ESP_LOGI(TAG, "KeyService polling loop active.");

    while (m_running) {
        for (int i = 0; i < 5; ++i) {
            bool pressed = m_input.isPressed(keys[i]);

            if (pressed) {
                if (!m_prevState[i]) {
                    m_prevState[i] = true;
                    m_pressCount[i] = 0;
                    m_longPressedTriggered[i] = false;
                    if (keys[i] == KeyId::KEY_2) {
                        m_key2StopConsumedThisPress = false;
                        m_key2Sleep = false;
                    }
                    ESP_LOGI(TAG, "KEY_%d PRESSED", i + 1);

                    // While an alarm rings or is snoozed, the keys belong to it:
                    // Key 2 stops it, any other key snoozes it.
                    m_alarmPress[i] = sysdb.snapshot().alarm.state != AlarmRingState::IDLE;
                    if (m_alarmPress[i]) {
                        const bool stop = keys[i] == KeyId::KEY_2;
                        ESP_LOGI(TAG, "Key %d: %s the alarm", i + 1, stop ? "stopping" : "snoozing");
                        sysdb.mutate([stop](SystemState& s) {
                            if (stop) s.alarm.stop_requested = true;
                            else s.alarm.snooze_requested = true;
                        });
                    }
                    // Key 4: toggle Play/Pause on press down
                    else if (keys[i] == KeyId::KEY_4) {
                        ESP_LOGI("KeySvc", "Key 4: toggling playback");
                        MusicPlaybackService::getInstance().postCommand(MediaCmdType::TOGGLE_PLAY_PAUSE);
                    }
                    // Key 2: stop any active recording on press down
                    else if (keys[i] == KeyId::KEY_2) {
                        if (AudioRecorder::getInstance().isRecording()) {
                            ESP_LOGI("KeySvc", "Key 2 press: stopping active recording");
                            AudioRecorder::getInstance().stopRecording(AudioRecorder::StopReason::MANUAL);
                            m_key2StopConsumedThisPress = true;
                        }
                    }
                } else {
                    m_pressCount[i]++;
                    // Any KEY_2 press, an alarm's or a recording's included,
                    // can be held on into sleep.
                    if (keys[i] == KeyId::KEY_2) key2Hold(m_pressCount[i]);
                    // Standard long press threshold (500ms / 25 ticks of 20ms)
                    if (m_pressCount[i] >= 25 && !m_longPressedTriggered[i] && !m_alarmPress[i]) {
                        m_longPressedTriggered[i] = true;
                        ESP_LOGI(TAG, "KEY_%d LONG PRESSED (500ms)", i + 1);

                        if (keys[i] == KeyId::KEY_3) {
                            ESP_LOGI(TAG, "Key 3 long press: requesting next track");
                            MusicPlaybackService::getInstance().postCommand(MediaCmdType::NEXT);
                        } else if (keys[i] == KeyId::KEY_5) {
                            ESP_LOGI(TAG, "Key 5 long press: requesting previous track");
                            MusicPlaybackService::getInstance().postCommand(MediaCmdType::PREVIOUS);
                        }
                        // KEY_2: decided on release (it may become a sleep hold).
                    }
                }
            } else {
                if (m_prevState[i]) {
                    m_prevState[i] = false;
                    ESP_LOGI(TAG, "KEY_%d RELEASED", i + 1);

                    if (keys[i] == KeyId::KEY_2) {
                        if (m_key2Sleep) Services::DeepSleep::enter();
                        LedService::setPowerHold(-1);
                        // Held past the long press but not into sleep: stereo recording.
                        if (m_longPressedTriggered[i] && !m_alarmPress[i] && !m_key2StopConsumedThisPress) {
                            ESP_LOGI(TAG, "Key 2 long press: starting STEREO recording");
                            AudioRecorder::getInstance().startRecording(AudioRecorder::RecordMode::STEREO);
                        }
                    }

                    // Short press logic: volume up/down if long press was NOT triggered
                    if (!m_longPressedTriggered[i] && !m_alarmPress[i]) {
                        if (keys[i] == KeyId::KEY_3) {
                            sysdb.mutate([](SystemState& s) {
                                int old_vol = s.audio.speaker_volume;
                                s.audio.speaker_volume = std::min(old_vol + 5, 100);
                                ESP_LOGI("KeySvc", "Volume increase request: %d -> %d", old_vol, s.audio.speaker_volume);
                            });
                        } else if (keys[i] == KeyId::KEY_5) {
                            sysdb.mutate([](SystemState& s) {
                                int old_vol = s.audio.speaker_volume;
                                s.audio.speaker_volume = std::max(old_vol - 5, 0);
                                ESP_LOGI("KeySvc", "Volume decrease request: %d -> %d", old_vol, s.audio.speaker_volume);
                            });
                        } else if (keys[i] == KeyId::KEY_2) {
                            if (!m_key2StopConsumedThisPress) {
                                ESP_LOGI(TAG, "Key 2 short press: starting PROCESSED recording");
                                AudioRecorder::getInstance().startRecording(AudioRecorder::RecordMode::PROCESSED);
                            }
                        }
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}