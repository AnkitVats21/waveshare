#pragma once

#include "common/app_types.h"
#include "driver/i2c_master.h"
#include "hal/Board.h"
#include "hal/HalBase.h"

/**
 * @brief Hardware Abstraction Layer for Audio
 * Manages handles and settings for the ES8311 and ES7210 codecs.
 * This is a singleton representing the physical audio hardware.
 */
class AudioHalManager : public HalBase {
public:
  static AudioHalManager &getInstance();

  /**
   * @brief Initialize audio hardware and link handles to the system context.
   */
  bool begin(const GlobalSystemSettings &settings,
             HardwareAudioHandles &out_handles,
             Board *board);

  // Default begin override (not recommended for this specific HAL)
  bool begin() override { return false; }

  // Volume and Gain Control (Thread-safe public API)
  esp_err_t setSpeakerVolume(int volume);
  esp_err_t getSpeakerVolume(int *volume);
  esp_err_t setMicGain(float db_value);

private:
  AudioHalManager() = default;
  ~AudioHalManager() = default;

  const GlobalSystemSettings *m_settings = nullptr;
  HardwareAudioHandles *m_handles = nullptr;
  Board *m_board = nullptr;

  static constexpr const char *TAG = "AudioHal";
};
