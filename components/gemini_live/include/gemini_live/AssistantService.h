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
    // When the current session started listening after connecting (0 = not
    // yet): the silence timeout counts from there, not from the wake.
    static int64_t listeningSinceUs() { return s_listening_us.load(); }
    // Ends the session once the model has finished its next turn (the reply
    // to a tool call) and spoken it, then starts a new one (a profile
    // switch: the setup is fixed per connection). No session-end or wake
    // chime; the ready chime says the new one listens. silence_ms: its
    // silence timeout (0 = the wake word's). From the tool handler.
    static void requestRestart(uint32_t silence_ms);

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
    static void restartWakeCallback(void* arg);

    void handleConnectTimeout();
    void handleIdleTimeout();
    void handleCooldownElapsed();
    void maybeRestart(const SystemState& snap);
    void restartWake();

    AssistantState m_current_state = AssistantState::Idle;

    esp_timer_handle_t m_idle_timer = nullptr;
    esp_timer_handle_t m_connect_timer = nullptr;
    esp_timer_handle_t m_cooldown_timer = nullptr;
    esp_timer_handle_t m_restart_timer = nullptr;   // retries the restart's wake
    int64_t m_wake_us = 0;   // when the current session was woken
    bool m_quiet_session = false;   // this session started quiet (requestQuietWake)
    static constexpr int64_t QUIET_WAKE_US = 3 * 1000 * 1000;
    static inline std::atomic<int64_t> s_quiet_wake_us{0};
    static inline std::atomic<int64_t> s_listening_us{0};
    // requestRestart: the silence timeout, -1 = none asked. m_restarting from
    // the quiet close until the new session starts (or the wake gives up).
    static inline std::atomic<int32_t> s_restart_silence_ms{-1};
    static inline std::atomic<uint32_t> s_restart_after_turn{0};   // turnsCompleted() at the request
    bool m_restarting = false;
    uint32_t m_restart_silence_ms = 0;
    int m_restart_tries = 0;
    static constexpr uint64_t RESTART_RETRY_US = 100ULL * 1000;
    static constexpr int RESTART_MAX_TRIES = 30;
    volatile bool m_restart_wake_pending = false;

    volatile bool m_connect_timeout_pending   = false;
    volatile bool m_idle_timeout_pending      = false;
    volatile bool m_cooldown_elapsed_pending  = false;
    volatile bool m_pending_idle_transition   = false; ///< Deferred Closing→Idle to avoid recursive transitionTo()

    static constexpr const char* TAG = "AssistantSvc";
};
