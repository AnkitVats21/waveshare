#pragma once

#include "common/ReactorTask.h"
#include "common/app_types.h"
#include "esp_timer.h"
#include <atomic>

class AssistantService : public ReactorTask {
public:
    AssistantService();
    ~AssistantService() override;

    bool begin();

    // ReactorTask interface
    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

    // The next session started within QUIET_WAKE_US plays no wake or ready
    // chime (a scheduled action opening the session). Any task.
    static void requestQuietWake() { s_quiet_wake_us = esp_timer_get_time(); }
    static void cancelQuietWake() { s_quiet_wake_us = 0; }

protected:

private:
    static constexpr uint64_t SESSION_FOLLOWUP_TIMEOUT_US = 60ULL * 1000 * 1000;
    static constexpr uint64_t CONNECT_TIMEOUT_US          = 10ULL * 1000 * 1000;
    static constexpr uint64_t COOLDOWN_TIMEOUT_US         = 5ULL * 1000 * 1000;

    void executeTransition(AssistantState newState, const SystemState& snap, bool is_external_sync);
    void transitionTo(AssistantState newState, const SystemState* snap_ptr = nullptr);

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
    int64_t m_wake_us = 0;   // when the current session was woken
    bool m_quiet_session = false;   // this session started quiet (requestQuietWake)
    static constexpr int64_t QUIET_WAKE_US = 3 * 1000 * 1000;
    static inline std::atomic<int64_t> s_quiet_wake_us{0};

    volatile bool m_connect_timeout_pending   = false;
    volatile bool m_idle_timeout_pending      = false;
    volatile bool m_cooldown_elapsed_pending  = false;
    volatile bool m_pending_idle_transition   = false; ///< Deferred Closing→Idle to avoid recursive transitionTo()

    static constexpr const char* TAG = "AssistantSvc";
};
