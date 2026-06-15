#pragma once

#include "common/ReactorTask.h"
#include "common/app_types.h"
#include "esp_timer.h"

class AssistantService : public ReactorTask {
public:
    AssistantService();
    ~AssistantService() override;

    bool begin();

    // ReactorTask interface
    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

protected:
    void run() override;

private:
    void handleStateTransition(AssistantState oldState, AssistantState newState, const SystemState& snap);
    void transitionTo(AssistantState newState);
    void publishMusicCommand(AssistantState oldState, AssistantState newState);

    static void connectTimeoutCallback(void* arg);
    static void idleTimeoutCallback(void* arg);
    static void cooldownTimeoutCallback(void* arg);

    void handleConnectTimeout();
    void handleIdleTimeout();
    void handleCooldownElapsed();

    AssistantState m_current_state = AssistantState::Idle;

    esp_timer_handle_t m_idle_timer = nullptr;
    esp_timer_handle_t m_connect_timer = nullptr;
    esp_timer_handle_t m_cooldown_timer = nullptr;

    volatile bool m_connect_timeout_pending = false;
    volatile bool m_idle_timeout_pending = false;
    volatile bool m_cooldown_elapsed_pending = false;

    static constexpr const char* TAG = "AssistantSvc";
};
