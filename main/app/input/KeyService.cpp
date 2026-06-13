#include "KeyService.h"
#include "esp_log.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "common/thread_config.h"
#include <algorithm>

KeyService::KeyService(ExpanderKeyInput &input)
    : ReactorTask({
          "key_svc",
          ThreadConfig::StackSize::STACK_SMALL,
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

            if (pressed != m_prevState[i]) {
                m_prevState[i] = pressed;

                if (pressed) {
                    ESP_LOGI(TAG, "KEY_%d PRESSED", i + 1);

                    if (keys[i] == KeyId::KEY_3) {
                        EmbeddedSysDb::getInstance().mutate(COMP::AUDIO, [](SystemState& s) {
                            int old_vol = s.audio.speaker_volume;
                            s.audio.speaker_volume = std::min(old_vol + 5, 100);
                            ESP_LOGI("KeySvc", "Volume increase request: %d -> %d", old_vol, s.audio.speaker_volume);
                        });
                    } else if (keys[i] == KeyId::KEY_5) {
                        EmbeddedSysDb::getInstance().mutate(COMP::AUDIO, [](SystemState& s) {
                            int old_vol = s.audio.speaker_volume;
                            s.audio.speaker_volume = std::max(old_vol - 5, 0);
                            ESP_LOGI("KeySvc", "Volume decrease request: %d -> %d", old_vol, s.audio.speaker_volume);
                        });
                    }
                } else {
                    ESP_LOGI(TAG, "KEY_%d RELEASED", i + 1);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}