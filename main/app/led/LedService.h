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

    // Shows a level (0-100) on the ring in `color` for a moment, over
    // whatever the ring shows (the volume keys). Any task.
    static void showLevel(int percent, RgbColor color);

    // ReactorTask interface
    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

protected:
    void run() override;

private:
    LedStripManager& m_leds;

    SemaphoreHandle_t m_cmd_mutex = nullptr;
    LedEventData      m_current_command{};

    void applyVisualState(AssistantVisualState state, const SystemState& snap);

    // showLevel's, under m_cmd_mutex; shown until until_us.
    struct Level {
        int percent = 0;
        RgbColor color{};
        int64_t until_us = 0;
    };
    Level m_level;

    static LedService* s_instance;
    static constexpr int64_t LEVEL_SHOW_US = 1500 * 1000;

    static constexpr const char* TAG = "LedSvc";
};
