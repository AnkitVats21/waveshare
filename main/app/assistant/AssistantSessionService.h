#pragma once

#include "common/IService.h"
#include "app/assistant/AssistantEvents.h"
#include "app/assistant/AssistantVisualState.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h" // Native FreeRTOS Mutual Exclusion

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

    // Zero-overhead state retrieval utilizing native memory-fence reads
    AssistantState getState() const { 
        return static_cast<AssistantState>(__atomic_load_n(&m_state_atomic, __ATOMIC_RELAXED)); 
    }
    const char* getStateName(AssistantState state) const;

private:
    AssistantSessionService();
    ~AssistantSessionService() = default;

    // Singleton constraints
    AssistantSessionService(const AssistantSessionService&) = delete;
    AssistantSessionService& operator=(const AssistantSessionService&) = delete;

    void handleAssistantEvent(AssistantEvent event, void* data);
    void transitionTo(AssistantState newState);

    // Native FreeRTOS Kernel Mutex Handle
    SemaphoreHandle_t m_rtos_mutex;

    // Standard types backed exclusively by GCC atomic machine instructions
    int m_wifi_available_atomic;
    int m_state_atomic;
    
    // ESP-IDF Timer Handles for session states
    esp_timer_handle_t m_idle_timer = nullptr;
    esp_timer_handle_t m_connect_timer = nullptr;
    esp_timer_handle_t m_cooldown_timer = nullptr;

    static constexpr const char* TAG = "AssistSessionSvc";
};