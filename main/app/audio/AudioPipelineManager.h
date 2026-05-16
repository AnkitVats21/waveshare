#pragma once

#include "common/app_types.h"

/**
 * @brief Manages the initialization and orchestration of the audio pipeline
 */
class AudioPipelineManager {
public:
  /**
   * @brief Initialize the audio pipeline subsystems
   * @param settings System settings
   * @param hw_handles Pre-initialized hardware handles
   * @param out_context Pipeline context to be populated
   * @return true if successful
   */
  static bool initialize(const GlobalSystemSettings &settings,
                         const HardwareAudioHandles &hw_handles,
                         GlobalPipelineContext &out_context);
};
