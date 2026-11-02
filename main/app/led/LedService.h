#pragma once

#include "common/ReactorTask.h"
#include "common/thread_config.h"
#include "common/led_types.h"
#include "common/app_types.h"
#include <atomic>

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

    // KEY_2 power hold: lights `lit` of the ring's LEDs (0 = all off) over
    // whatever the ring shows; -1 gives the ring back. Any task.
    static void setPowerHold(int lit);

    // ReactorTask interface
    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

protected:
    void run() override;

private:
    LedStripManager& m_leds;

    SemaphoreHandle_t m_cmd_mutex = nullptr;
    LedEventData      m_current_command{};

    void applyVisualState(AssistantVisualState state, const SystemState& snap);

    static LedService*      s_instance;
    static std::atomic<int> s_power_hold;

    static constexpr const char* TAG = "LedSvc";
};
