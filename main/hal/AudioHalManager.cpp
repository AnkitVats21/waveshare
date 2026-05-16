#include "AudioHalManager.h"
#include "common/AppLogger.h"
#include "hal/Board.h"

AudioHalManager &AudioHalManager::getInstance() {
  static AudioHalManager instance;
  return instance;
}

bool AudioHalManager::begin(const GlobalSystemSettings &settings,
                             HardwareAudioHandles &out_handles,
                             Board *board) {
  if (m_initialized) return true;

  m_settings = &settings;
  m_handles = &out_handles;
  m_board = board;

  if (!m_board) {
      LOGE_HAL("Board pointer is null in AudioHalManager!");
      return false;
  }

  // Ensure hardware bring-up is complete
  if (!m_board->isInitialized()) {
      if (!m_board->begin()) {
          LOGE_HAL("Failed to initialize physical board for AudioHal.");
          return false;
      }
  }

  // Map low-level driver handles to our system context
  m_handles->speaker_tx_handle = m_board->getTxHandle();
  m_handles->mic_rx_handle = m_board->getRxHandle();
  m_handles->play_dev = m_board->getPlayDev();
  m_handles->record_dev = m_board->getRecordDev();

  LOGI_HAL("Audio HAL Manager operational. Hardware handles mapped.");
  m_initialized = true;
  return true;
}

esp_err_t AudioHalManager::setSpeakerVolume(int volume) {
    if (m_board) {
        return m_board->setPlayVolume(volume);
    }
    return ESP_FAIL;
}

esp_err_t AudioHalManager::getSpeakerVolume(int *volume) {
    if (m_board) {
        return m_board->getPlayVolume(volume);
    }
    return ESP_FAIL;
}

esp_err_t AudioHalManager::setMicGain(float db_value) {
    if (m_board) {
        return m_board->setRecordGain(db_value);
    }
    return ESP_FAIL;
}
