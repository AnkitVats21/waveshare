#include "app/led/LedService.h"
#include "common/AppLogger.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "hal/led/LedStripManager.h"
#include "hal/Board_defs.h"
#include "esp_log.h"
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

LedService::LedService(LedStripManager& leds)
    : ReactorTask({
          "led_svc",
          ThreadConfig::STACK_SMALL,
          ThreadConfig::LED,
          ThreadConfig::CORE_AUDIO,   // Core 1 — low priority, no contention
          COMP::ASSISTANT | COMP::LED
      })
    , m_leds(leds)
{
    m_cmd_mutex = xSemaphoreCreateMutex();
    m_current_command.mode         = LedMode::OFF;
    m_current_command.color        = OFF_LED;
    m_current_command.speed_ms     = 0;
    m_current_command.repeat_count = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReactorTask::onStateChanged — fast path
// ─────────────────────────────────────────────────────────────────────────────

void LedService::onStateChanged(ComponentMask changed, const SystemState& snap) {
    if (changed & COMP::ASSISTANT) {
        applyVisualState(snap.assistant.visual_state, snap);
    }
    if (changed & COMP::LED) {
        if (m_cmd_mutex && xSemaphoreTake(m_cmd_mutex, portMAX_DELAY) == pdTRUE) {
            m_current_command.mode = snap.led.mode;
            m_current_command.color = snap.led.color;
            m_current_command.speed_ms = snap.led.speed_ms;
            m_current_command.repeat_count = snap.led.repeat;
            xSemaphoreGive(m_cmd_mutex);
        }
    }
}

void LedService::applyVisualState(AssistantVisualState state, const SystemState& snap) {
    if (m_cmd_mutex && xSemaphoreTake(m_cmd_mutex, portMAX_DELAY) == pdTRUE) {
        switch (state) {
            case AssistantVisualState::Idle:
                // Retain/restore the custom color set by tool calling or MQTT when Idle
                m_current_command = {snap.led.mode, snap.led.color, snap.led.speed_ms, snap.led.repeat};
                ESP_LOGI(TAG, "LED State: Idle (restoring color: R=%d, G=%d, B=%d, Mode=%d)", 
                         snap.led.color.r, snap.led.color.g, snap.led.color.b, (int)snap.led.mode);
                break;
            case AssistantVisualState::Listening:
                m_current_command = {LedMode::SOLID, GREEN_LED, 0, 0};
                ESP_LOGI(TAG, "LED State: Listening (Green Solid)");
                break;
            case AssistantVisualState::Connecting:
                m_current_command = {LedMode::SOLID, BLUE_LED, 0, 0};
                ESP_LOGI(TAG, "LED State: Connecting (Blue Solid)");
                break;
            case AssistantVisualState::Speaking:
                m_current_command = {LedMode::SOLID, PURPLE_LED, 0, 0};
                ESP_LOGI(TAG, "LED State: Speaking (Purple Solid)");
                break;
            case AssistantVisualState::Thinking:
                m_current_command = {LedMode::SOLID, BLUE_LED, 0, 0};
                ESP_LOGI(TAG, "LED State: Thinking (Blue Solid)");
                break;
            case AssistantVisualState::Offline:
                m_current_command = {LedMode::SOLID, RED_LED, 0, 0};
                ESP_LOGI(TAG, "LED State: Offline (Red Solid)");
                break;
            case AssistantVisualState::Recovering:
                m_current_command = {LedMode::SOLID, ORANGE_LED, 0, 0};
                ESP_LOGI(TAG, "LED State: Recovering (Orange Solid)");
                break;
            case AssistantVisualState::RateLimited:
                m_current_command = {LedMode::SOLID, PURPLE_LED, 0, 0};
                ESP_LOGI(TAG, "LED State: Rate Limited (Purple Solid)");
                break;
            case AssistantVisualState::Error:
                m_current_command = {LedMode::SOLID, RED_LED, 0, 0};
                ESP_LOGI(TAG, "LED State: Error (Red Solid)");
                break;
        }
        xSemaphoreGive(m_cmd_mutex);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Animation loop (ReactorTask::run)
// ─────────────────────────────────────────────────────────────────────────────

void LedService::run() {
    uint32_t step        = 0;
    bool     blink_state = false;
    float    breath_val  = 0.0f;
    LedMode  last_mode   = LedMode::OFF;

    ESP_LOGI(TAG, "LedService animation loop active.");

    while (m_running) {
        if (!m_leds.isInitialized()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        LedEventData cmd;
        if (m_cmd_mutex && xSemaphoreTake(m_cmd_mutex, portMAX_DELAY) == pdTRUE) {
            cmd = m_current_command;
            xSemaphoreGive(m_cmd_mutex);
        }

        if (cmd.mode != last_mode) {
            step        = 0;
            blink_state = false;
            breath_val  = 0.0f;
            last_mode   = cmd.mode;
        }

        // 1. Physically apply/draw the current LED state first
        switch (cmd.mode) {
            case LedMode::OFF:
                m_leds.clear();
                break;

            case LedMode::SOLID:
                m_leds.setAll(cmd.color.r, cmd.color.g, cmd.color.b);
                m_leds.refresh();
                break;

            case LedMode::BLINK:
                if (cmd.repeat_count > 0 && step >= (uint32_t)cmd.repeat_count * 2) {
                    m_leds.clear();
                    if (m_cmd_mutex && xSemaphoreTake(m_cmd_mutex, portMAX_DELAY) == pdTRUE) {
                        m_current_command.mode = LedMode::OFF;
                        xSemaphoreGive(m_cmd_mutex);
                    }
                    last_mode = LedMode::OFF;
                    cmd.mode = LedMode::OFF;
                    break;
                }
                if (blink_state) {
                    m_leds.setAll(cmd.color.r, cmd.color.g, cmd.color.b);
                    m_leds.refresh();
                } else {
                    m_leds.clear();
                }
                blink_state = !blink_state;
                step++;
                break;

            case LedMode::BREATH: {
                float factor = sinf(breath_val);
                if (factor < 0) factor = -factor;

                m_leds.setAll(
                    (uint8_t)(cmd.color.r * factor),
                    (uint8_t)(cmd.color.g * factor),
                    (uint8_t)(cmd.color.b * factor));
                m_leds.refresh();

                breath_val += 0.08f;
                if (breath_val >= M_PI) {
                    breath_val = 0.0f;
                    if (cmd.repeat_count > 0 && ++step >= (uint32_t)cmd.repeat_count) {
                        m_leds.clear();
                        if (m_cmd_mutex && xSemaphoreTake(m_cmd_mutex, portMAX_DELAY) == pdTRUE) {
                            m_current_command.mode = LedMode::OFF;
                            xSemaphoreGive(m_cmd_mutex);
                        }
                        last_mode = LedMode::OFF;
                        cmd.mode = LedMode::OFF;
                    }
                }
                break;
            }

            case LedMode::RAINBOW: {
                for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
                    uint8_t hue = (step + (i * 256 / LED_STRIP_LED_COUNT)) & 255;
                    uint8_t r = 0, g = 0, b = 0;
                    if (hue < 85) {
                        r = hue * 3; g = 255 - hue * 3; b = 0;
                    } else if (hue < 170) {
                        hue -= 85; r = 255 - hue * 3; g = 0; b = hue * 3;
                    } else {
                        hue -= 170; r = 0; g = hue * 3; b = 255 - hue * 3;
                    }
                    m_leds.setPixel(i, r / 4, g / 4, b / 4);
                }
                m_leds.refresh();
                step      = (step + 5) & 255;
                break;
            }

            default:
                break;
        }

        // 2. Calculate delay and wait for the next frame or database notification
        uint32_t delay_ms = 100;
        switch (cmd.mode) {
            case LedMode::OFF:        delay_ms = 200; break;
            case LedMode::SOLID:      delay_ms = 200; break;
            case LedMode::BLINK:      delay_ms = (cmd.speed_ms > 0) ? cmd.speed_ms : 500; break;
            case LedMode::BREATH:     delay_ms = (cmd.speed_ms > 0) ? cmd.speed_ms : 30; break;
            case LedMode::RAINBOW:    delay_ms = (cmd.speed_ms > 0) ? cmd.speed_ms : 30; break;
            default: break;
        }

        uint32_t changed_bits = 0;
        TickType_t wait_ticks = (cmd.mode == LedMode::OFF || cmd.mode == LedMode::SOLID) ? portMAX_DELAY : pdMS_TO_TICKS(delay_ms);
        BaseType_t notified = xTaskNotifyWait(0, 0xFFFFFFFF, &changed_bits, wait_ticks);
        if (!m_running) break;

        if (notified == pdTRUE && changed_bits > 0) {
            m_last_changed = changed_bits;
            SystemState snap = EmbeddedSysDb::getInstance().snapshot();
            onStateChanged(m_last_changed, snap);
        }
    }
}
