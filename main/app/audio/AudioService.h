#pragma once

#include "common/TaskBase.h"
#include "common/app_types.h"
#include "esp_event.h"
#include "hal/HalBase.h"

// Forward declarations
class Board;
class EventBus;
class AudioHalManager;

/**
 * @brief Unified Audio Service
 * Coordinates the entire audio lifecycle (Mic, Speaker, Pipeline)
 * and reacts to system events.
 */
class AudioService : public HalBase, public TaskBase {
public:
  static AudioService &getInstance();

  /**
   * @brief Initialize the audio service and its sub-managers
   */
  bool begin(GlobalSystemSettings &settings, GlobalPipelineContext &context,
             HardwareAudioHandles &handles, Board *board, EventBus *event_bus);

  bool begin() override { return false; }

protected:
  /**
   * @brief Background supervisory loop
   */
  void run() override;

private:
  AudioService() : TaskBase({"AudioService", 8192, 5, 0}) {}
  ~AudioService() = default;

  // Event Handlers
  static void onSystemEvent(void *handler_arg, esp_event_base_t base,
                            int32_t id, void *event_data);

  GlobalSystemSettings *m_settings = nullptr;
  GlobalPipelineContext *m_context = nullptr;
  HardwareAudioHandles *m_handles = nullptr;
  Board *m_board = nullptr;
  EventBus *m_event_bus = nullptr;

  static constexpr const char *TAG = "AudioSvc";
};
