#include "KeyService.h"
#include "esp_log.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "common/thread_config.h"
#include "app/media_player/NexusPlayer.h"
#include "app/media_player/MusicPlaybackService.h"
#include "app/audio/recording/AudioRecorder.h"
#include "freertos/idf_additions.h"
#include <algorithm>

static auto& sysdb = EmbeddedSysDb::getInstance();

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
                    }
                    ESP_LOGI(TAG, "KEY_%d PRESSED", i + 1);

                    // Key 4: toggle Play/Pause on press down
                    if (keys[i] == KeyId::KEY_4) {
                        ESP_LOGI("KeySvc", "Key 4: toggling playback");
                        MusicPlaybackService::getInstance().postCommand(MediaCmdType::TOGGLE_PLAY_PAUSE);
                    }
                    // Key 2: stop alarm on press down, and stop any active recording
                    else if (keys[i] == KeyId::KEY_2) {
                        sysdb.mutate([](SystemState& s) {
                            if (s.alarm.playing) {
                                s.alarm.stop_requested = true;
                                ESP_LOGI("KeySvc", "Alarm stop requested via Key 2");
                            }
                        });
                        if (AudioRecorder::getInstance().isRecording()) {
                            ESP_LOGI("KeySvc", "Key 2 press: stopping active recording");
                            AudioRecorder::getInstance().stopRecording(AudioRecorder::StopReason::MANUAL);
                            m_key2StopConsumedThisPress = true;
                        }
                    }
                } else {
                    m_pressCount[i]++;
                    // Standard long press threshold (500ms / 25 ticks of 20ms)
                    if (m_pressCount[i] >= 25 && !m_longPressedTriggered[i]) {
                        m_longPressedTriggered[i] = true;
                        ESP_LOGI(TAG, "KEY_%d LONG PRESSED (500ms)", i + 1);

                        if (keys[i] == KeyId::KEY_3) {
                            ESP_LOGI(TAG, "Key 3 long press: requesting next track");
                            MusicPlaybackService::getInstance().postCommand(MediaCmdType::NEXT);
                        } else if (keys[i] == KeyId::KEY_5) {
                            ESP_LOGI(TAG, "Key 5 long press: requesting previous track");
                            MusicPlaybackService::getInstance().postCommand(MediaCmdType::PREVIOUS);
                        } else if (keys[i] == KeyId::KEY_2) {
                            if (!m_key2StopConsumedThisPress) {
                                ESP_LOGI(TAG, "Key 2 long press: starting RAW recording");
                                AudioRecorder::getInstance().startRecording(AudioRecorder::RecordMode::RAW);
                            }
                        }
                    }
                }
            } else {
                if (m_prevState[i]) {
                    m_prevState[i] = false;
                    ESP_LOGI(TAG, "KEY_%d RELEASED", i + 1);

                    // Short press logic: volume up/down if long press was NOT triggered
                    if (!m_longPressedTriggered[i]) {
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
                                ESP_LOGI(TAG, "Key 2 short press: starting RESAMPLED recording");
                                AudioRecorder::getInstance().startRecording(AudioRecorder::RecordMode::RESAMPLED);
                            }
                        }
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}