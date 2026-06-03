#pragma once

#include "common/IService.h"
#include "app/assistant/AssistantEvents.h"
#include "app/assistant/AssistantVisualState.h"
#include "esp_timer.h"
#include <atomic>

#include <mutex>

/**
 * @brief States representing the assistant's conversation session.
 */
enum class AssistantState {
    Idle,
    StartingSession,
    Connecting,
    StreamingUserAudio,
    AssistantSpeaking,
    WaitingForFollowup,
    Closing,
    ErrorCooldown
};

/**
 * @brief Central controller managing the assistant conversation lifecycle and state machine.
 */
class AssistantSessionService : public IService {
public:
    static AssistantSessionService& getInstance();

    bool onStart() override;
    void onStop() override;
    void onEvent(esp_event_base_t base, int32_t id, void* data) override;

    AssistantState getState() const { return m_state.load(std::memory_order_relaxed); }
    const char* getStateName(AssistantState state) const;

private:
    AssistantSessionService();
    ~AssistantSessionService() = default;

    // Singleton constraints
    AssistantSessionService(const AssistantSessionService&) = delete;
    AssistantSessionService& operator=(const AssistantSessionService&) = delete;

    void handleAssistantEvent(AssistantEvent event, void* data);
    void transitionTo(AssistantState newState);

    std::atomic<AssistantState> m_state{AssistantState::Idle};
    std::mutex m_mutex;

    bool m_wifi_available = false;
    
    // ESP-IDF Timer Handles for session states
    esp_timer_handle_t m_idle_timer = nullptr;
    esp_timer_handle_t m_connect_timer = nullptr;
    esp_timer_handle_t m_cooldown_timer = nullptr;

    static constexpr const char* TAG = "AssistSessionSvc";
};

