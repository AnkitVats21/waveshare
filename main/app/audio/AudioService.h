#pragma once

#include "common/IService.h"
#include "common/TaskBase.h"
#include "common/hw_types.h"
#include "common/system_settings.h"
#include "app/wake_word/IWakeWordListener.h"
#include "services/BufferManager.h"

class Board;

/**
 * @brief Unified Audio Service.
 *
 * Extends IService  — standardized event subscription and dispatch.
 * Extends TaskBase  — background supervisor loop.
 * Implements IWakeWordListener — receives wake-word/VAD/barge-in callbacks
 *   directly from WakeWordDetector without EventBus round-trips.
 */
class AudioService : public IService, public TaskBase, public IWakeWordListener {
public:
  static AudioService &getInstance();

  bool begin(GlobalSystemSettings &settings, HardwareAudioHandles &handles,
             Board *board);

  bool reinit(uint32_t sample_rate);
  void setMicEnabled(bool enabled);
  bool isInitialized() const { return m_initialized; }

  // IService interface
  bool onStart() override;
  void onEvent(esp_event_base_t base, int32_t id, void *data) override;

  // IWakeWordListener interface
  void onWakeWord(uint8_t channel) override;
  void onVadTimeout() override;
  void onUserSpeechDetected() override;

  void updateActivity();
  void checkInactivityTimeout();

protected:
  void run() override;

private:
  AudioService() : IService("AudioSvc"), TaskBase({"AudioService", 8192, 5, 0}) {}

  GlobalSystemSettings *m_settings = nullptr;
  HardwareAudioHandles *m_handles  = nullptr;
  Board                *m_board    = nullptr;

  bool     m_initialized      = false;
  float    m_mic_gain         = 60.0f;
  uint64_t m_last_activity_ms = 0;
  bool     m_session_active   = false;
  volatile bool m_turn_complete_pending = false;

  static constexpr const char *TAG = "AudioSvc";
};
