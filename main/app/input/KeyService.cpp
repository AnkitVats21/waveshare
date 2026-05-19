#include "KeyService.h"
#include "esp_log.h"
#include "hal/Board.h"

KeyService::KeyService(ExpanderKeyInput &input) : m_input(input) {}

void KeyService::start() {
  xTaskCreatePinnedToCore(taskEntry, "key_service", 4096, this, 5, nullptr, 1);
}

void KeyService::taskEntry(void *arg) {
  static_cast<KeyService *>(arg)->taskLoop();
}

void KeyService::taskLoop() {
  using KeyId = ExpanderKeyInput::KeyId;

  const KeyId keys[5] = {
      KeyId::KEY_1, KeyId::KEY_2, KeyId::KEY_3, KeyId::KEY_4, KeyId::KEY_5,
  };

  while (true) {

    for (int i = 0; i < 5; ++i) {

      bool pressed = m_input.isPressed(keys[i]);

      if (pressed != m_prevState[i]) {

        m_prevState[i] = pressed;

        if (pressed) {
          ESP_LOGI(TAG, "KEY_%d PRESSED", i + 1);

          if (keys[i] == KeyId::KEY_3) {
            int vol = 0;
            if (Board::getInstance().getPlayVolume(&vol) == ESP_OK) {
              int new_vol = vol + 5;
              if (new_vol > 100) new_vol = 100;
              Board::getInstance().setPlayVolume(new_vol);
              ESP_LOGI(TAG, "Volume increased: %d -> %d", vol, new_vol);
            } else {
              ESP_LOGE(TAG, "Failed to get current speaker volume");
            }
          } else if (keys[i] == KeyId::KEY_5) {
            int vol = 0;
            if (Board::getInstance().getPlayVolume(&vol) == ESP_OK) {
              int new_vol = vol - 5;
              if (new_vol < 0) new_vol = 0;
              Board::getInstance().setPlayVolume(new_vol);
              ESP_LOGI(TAG, "Volume decreased: %d -> %d", vol, new_vol);
            } else {
              ESP_LOGE(TAG, "Failed to get current speaker volume");
            }
          }
        } else {
          ESP_LOGI(TAG, "KEY_%d RELEASED", i + 1);
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}