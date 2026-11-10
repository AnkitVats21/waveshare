#pragma once

#include "core_sysdb/ReactorTask.h"
#include "core_sysdb/thread_config.h"
#include "core_sysdb/led_types.h"
#include "core_sysdb/app_types.h"

class LedStripManager;

/**
 * @brief LED animation and visual-state orchestrator — ReactorTask.
 *
 * Replaces the old IService+TaskBase+EventBus architecture.
 *
 * Watches:
 *   COMP::ASSISTANT — visual_state changes drive animation
 *   COMP::LED       — direct LED command overrides (e.g. from a Gemini tool call)
 *
 * Injected:
 *   LedStripManager& — direct hardware driver (no Board pass-through)
 */
class LedService : public ReactorTask {
public:
    explicit LedService(LedStripManager& leds);

    // ReactorTask interface
    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

protected:
    void run() override;

private:
    LedStripManager& m_leds;

    SemaphoreHandle_t m_cmd_mutex = nullptr;
    LedEventData      m_current_command{};

    void applyVisualState(AssistantVisualState state, const SystemState& snap);

    static constexpr const char* TAG = "LedSvc";
};
