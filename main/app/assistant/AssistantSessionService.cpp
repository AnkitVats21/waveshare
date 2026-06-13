#include "AssistantSessionService.h"
#include "app/event/EventBus.h"
#include "app/audio/AudioAlertPlayer.h"
#include "app/gemini_live/GeminiProtocolTask.h"
#include "common/AppLogger.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h" // Native FreeRTOS Semaphore engine
#include <cstring>

#define SUBSCRIBE_ASSISTANT_EVENT(id) subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::id)
#define PUBLISH_ASSISTANT_EVENT(id, data) EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::id, data)

// ---------------------------------------------------------------------------
// Native FreeRTOS Lightweight C++ RAII Mutex Guard
// ---------------------------------------------------------------------------
class FreeRTOSLockGuard {
public:
    explicit FreeRTOSLockGuard(SemaphoreHandle_t mutex) : m_mutex(mutex) {
        if (m_mutex) {
            xSemaphoreTake(m_mutex, portMAX_DELAY);
        }
    }
    ~FreeRTOSLockGuard() {
        if (m_mutex) {
            xSemaphoreGive(m_mutex);
        }
    }
private:
    SemaphoreHandle_t m_mutex;
};

AssistantSessionService::AssistantSessionService() 
    : IService("AssistantSessionService"), 
      m_rtos_mutex(nullptr),
      m_wifi_available_atomic(0),
      m_state_atomic(static_cast<int>(AssistantState::Idle)) {
      
    // Initialize native FreeRTOS mutex primitive at boot
    m_rtos_mutex = xSemaphoreCreateMutex();
    assert(m_rtos_mutex != nullptr);
}

// ---------------------------------------------------------------------------
// Helper: play an audio alert on a tiny fire-and-forget FreeRTOS task.
// This prevents AudioAlertPlayer from blocking the ESP event loop task
// (which would delay VISUAL_STATE_CHANGED reaching LedService).
// ---------------------------------------------------------------------------
static void playAlertTask(void* arg) {
    auto fn = reinterpret_cast<void(*)()>(arg);
    if (fn) {
        fn();
    }
    vTaskDelete(nullptr);
}

static void playAlertAsync(void (*fn)()) {
    // Pinned explicitly to Core 1 alongside our high-speed Audio pipelines
    xTaskCreatePinnedToCore(playAlertTask, "alert_async", 3072, (void*)fn, 3, nullptr, 1);
}

AssistantSessionService& AssistantSessionService::getInstance() {
    static AssistantSessionService instance;
    return instance;
}

bool AssistantSessionService::onStart() {
    LOGI_SYSTEM("Starting AssistantSessionService...");

    // Create ESP-IDF timers
    if (!m_idle_timer) {
        esp_timer_create_args_t idle_args = {};
        idle_args.callback = [](void* /*arg*/) {
            EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::SESSION_IDLE_TIMEOUT, 0);
        };
        idle_args.arg = nullptr;
        idle_args.dispatch_method = ESP_TIMER_TASK;
        idle_args.name = "session_idle";
        esp_timer_create(&idle_args, &m_idle_timer);
    }
    
    if (!m_connect_timer) {
        esp_timer_create_args_t connect_args = {};
        connect_args.callback = [](void* /*arg*/) {
            EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::CONNECT_TIMEOUT, 0);
        };
        connect_args.arg = nullptr;
        connect_args.dispatch_method = ESP_TIMER_TASK;
        connect_args.name = "connect_timeout";
        esp_timer_create(&connect_args, &m_connect_timer);
    }
    
    if (!m_cooldown_timer) {
        esp_timer_create_args_t cooldown_args = {};
        cooldown_args.callback = [](void* /*arg*/) {
            EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::COOLDOWN_ELAPSED, 0);
        };
        cooldown_args.arg = nullptr;
        cooldown_args.dispatch_method = ESP_TIMER_TASK;
        cooldown_args.name = "error_cooldown";
        esp_timer_create(&cooldown_args, &m_cooldown_timer);
    }

    SUBSCRIBE_ASSISTANT_EVENT(WAKE_WORD_DETECTED);
    SUBSCRIBE_ASSISTANT_EVENT(WIFI_AVAILABLE);
    SUBSCRIBE_ASSISTANT_EVENT(WIFI_LOST);
    SUBSCRIBE_ASSISTANT_EVENT(USER_SPEECH_DETECTED);
    SUBSCRIBE_ASSISTANT_EVENT(VAD_TIMEOUT);
    SUBSCRIBE_ASSISTANT_EVENT(USER_INTERRUPTED);
    SUBSCRIBE_ASSISTANT_EVENT(WS_CONNECTED);
    SUBSCRIBE_ASSISTANT_EVENT(WS_CONNECT_FAILED);
    SUBSCRIBE_ASSISTANT_EVENT(WS_CLOSED);
    SUBSCRIBE_ASSISTANT_EVENT(GEMINI_GO_AWAY);
    SUBSCRIBE_ASSISTANT_EVENT(ASSISTANT_AUDIO_STARTED);
    SUBSCRIBE_ASSISTANT_EVENT(ASSISTANT_TURN_COMPLETE);
    SUBSCRIBE_ASSISTANT_EVENT(QUOTA_EXCEEDED);
    SUBSCRIBE_ASSISTANT_EVENT(SERVER_ERROR);
    SUBSCRIBE_ASSISTANT_EVENT(TRANSPORT_ERROR);
    SUBSCRIBE_ASSISTANT_EVENT(CONNECT_TIMEOUT);
    SUBSCRIBE_ASSISTANT_EVENT(SESSION_IDLE_TIMEOUT);
    SUBSCRIBE_ASSISTANT_EVENT(COOLDOWN_ELAPSED);

    __atomic_store_n(&m_state_atomic, static_cast<int>(AssistantState::Idle), __ATOMIC_RELAXED);
    PUBLISH_ASSISTANT_EVENT(VISUAL_STATE_CHANGED, AssistantVisualState::Offline);
    return true;
}

void AssistantSessionService::onStop() {
    LOGI_SYSTEM("Tearing down AssistantSessionService layers cleanly...");
    if (m_idle_timer) {
        esp_timer_stop(m_idle_timer);
        esp_timer_delete(m_idle_timer);
        m_idle_timer = nullptr;
    }
    if (m_connect_timer) {
        esp_timer_stop(m_connect_timer);
        esp_timer_delete(m_connect_timer);
        m_connect_timer = nullptr;
    }
    if (m_cooldown_timer) {
        esp_timer_stop(m_cooldown_timer);
        esp_timer_delete(m_cooldown_timer);
        m_cooldown_timer = nullptr;
    }
    if (m_rtos_mutex) {
        vSemaphoreDelete(m_rtos_mutex);
        m_rtos_mutex = nullptr;
    }
}

void AssistantSessionService::onEvent(esp_event_base_t base, int32_t id, void* data) {
    if (base == ASSISTANT_EVENTS) {
        handleAssistantEvent(static_cast<AssistantEvent>(id), data);
    }
}

const char* AssistantSessionService::getStateName(AssistantState state) const {
    switch (state) {
        case AssistantState::Idle: return "Idle";
        case AssistantState::StartingSession: return "StartingSession";
        case AssistantState::Connecting: return "Connecting";
        case AssistantState::StreamingUserAudio: return "StreamingUserAudio";
        case AssistantState::AssistantSpeaking: return "AssistantSpeaking";
        case AssistantState::WaitingForFollowup: return "WaitingForFollowup";
        case AssistantState::Closing: return "Closing";
        case AssistantState::ErrorCooldown: return "ErrorCooldown";
    }
    return "Unknown";
}

void AssistantSessionService::transitionTo(AssistantState newState) {
    // Dynamic atomic exchange utilizing standard GCC machine fences across cores
    int expect = __atomic_load_n(&m_state_atomic, __ATOMIC_RELAXED);
    while (expect != static_cast<int>(newState)) {
        if (__atomic_compare_exchange_n(&m_state_atomic, &expect, static_cast<int>(newState), 
                                        false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
            break;
        }
    }
    
    AssistantState oldState = static_cast<AssistantState>(expect);
    if (oldState == newState) {
        return;
    }

    LOGI_SYSTEM("State change execution: %s -> %s", getStateName(oldState), getStateName(newState));

    // 1. Cleanup timers/actions of the old state
    switch (oldState) {
        case AssistantState::Connecting:
            if (m_connect_timer) esp_timer_stop(m_connect_timer);
            break;
        case AssistantState::WaitingForFollowup:
            if (m_idle_timer) esp_timer_stop(m_idle_timer);
            break;
        case AssistantState::ErrorCooldown:
            if (m_cooldown_timer) esp_timer_stop(m_cooldown_timer);
            break;
        default:
            break;
    }

    // 2. Setup actions/timers of the new state
    AssistantVisualState visState = AssistantVisualState::Idle;
    bool trigger_auto_transition_to_idle = false;
    int wifi_status = __atomic_load_n(&m_wifi_available_atomic, __ATOMIC_RELAXED);

    switch (newState) {
        case AssistantState::Idle:
            visState = (wifi_status != 0) ? AssistantVisualState::Idle : AssistantVisualState::Offline;
            PUBLISH_ASSISTANT_EVENT(AUDIO_RETURN_TO_WAKE_MODE_16K, 0);
            // Persistent WebSocket: We do NOT publish TRANSPORT_CLOSE here.
            break;

        case AssistantState::StartingSession:
            visState = AssistantVisualState::Thinking;
            break;

        case AssistantState::Connecting:
            visState = AssistantVisualState::Connecting;
            PUBLISH_ASSISTANT_EVENT(TRANSPORT_CONNECT, 0);
            if (m_connect_timer) {
                esp_timer_start_once(m_connect_timer, 10ULL * 1000 * 1000); // 10s timeout
            }
            break;

        case AssistantState::StreamingUserAudio:
            visState = AssistantVisualState::Listening;
            PUBLISH_ASSISTANT_EVENT(AUDIO_ENTER_CONVERSATION_MODE, 0);
            PUBLISH_ASSISTANT_EVENT(TRANSPORT_SEND_BUFFERED_AUDIO, 0);
            PUBLISH_ASSISTANT_EVENT(TRANSPORT_SEND_LIVE_AUDIO, 0);
            break;

        case AssistantState::AssistantSpeaking:
            visState = AssistantVisualState::Speaking;
            PUBLISH_ASSISTANT_EVENT(AUDIO_ENTER_PLAYBACK_MODE_24K, 0);
            PUBLISH_ASSISTANT_EVENT(AUDIO_SUSPEND_MIC_STREAMING, 0);
            break;

        case AssistantState::WaitingForFollowup:
            visState = AssistantVisualState::Thinking;
            /* if (m_idle_timer) {
                esp_timer_start_once(m_idle_timer, 30ULL * 1000 * 1000); // 30s window
            } */
            break;

        case AssistantState::Closing:
            PUBLISH_ASSISTANT_EVENT(TRANSPORT_CLOSE, 0);
            PUBLISH_ASSISTANT_EVENT(AUDIO_RETURN_TO_WAKE_MODE_16K, 0);
            trigger_auto_transition_to_idle = true; 
            break;

        case AssistantState::ErrorCooldown:
            visState = AssistantVisualState::Error;
            PUBLISH_ASSISTANT_EVENT(TRANSPORT_CLOSE, 0);
            PUBLISH_ASSISTANT_EVENT(AUDIO_RETURN_TO_WAKE_MODE_16K, 0);
            if (m_cooldown_timer) {
                esp_timer_start_once(m_cooldown_timer, 5ULL * 1000 * 1000); // 5s cooldown
            }
            break;
    }

    // Publish visual state FIRST so LedService updates immediately
    PUBLISH_ASSISTANT_EVENT(VISUAL_STATE_CHANGED, visState);

    // Play audio alerts AFTER visual update safely inside background task windows
    switch (newState) {
        case AssistantState::StartingSession:
            playAlertAsync(AudioAlertPlayer::playWakeConfirm);
            break;
        case AssistantState::StreamingUserAudio:
            playAlertAsync(AudioAlertPlayer::playReadyToSpeak);
            break;
        case AssistantState::Closing:
            playAlertAsync(AudioAlertPlayer::playSessionEnd);
            break;
        case AssistantState::ErrorCooldown:
            playAlertAsync(AudioAlertPlayer::playError);
            break;
        default:
            break;
    }

    // Safely execute sequential transitions outside of active recursive lock windows
    if (trigger_auto_transition_to_idle) {
        this->transitionTo(AssistantState::Idle);
    }
}

// void AssistantSessionService::resetIdleTimer() {
//     FreeRTOSLockGuard lock(m_rtos_mutex);
//     if (m_idle_timer && static_cast<AssistantState>(__atomic_load_n(&m_state_atomic, __ATOMIC_RELAXED)) == AssistantState::WaitingForFollowup) {
//         esp_timer_stop(m_idle_timer);
//         esp_timer_start_once(m_idle_timer, 30ULL * 1000 * 1000); // 30s window
//     }
// }

void AssistantSessionService::handleAssistantEvent(AssistantEvent event, void* /*data*/) {
    // Native FreeRTOS lock guard allocation avoids the standard POSIX wrapper overhead entirely
    FreeRTOSLockGuard lock(m_rtos_mutex);
    
    if (event == AssistantEvent::WIFI_AVAILABLE) {
        __atomic_store_n(&m_wifi_available_atomic, 1, __ATOMIC_RELAXED);
        if (static_cast<AssistantState>(__atomic_load_n(&m_state_atomic, __ATOMIC_RELAXED)) == AssistantState::Idle) {
            PUBLISH_ASSISTANT_EVENT(VISUAL_STATE_CHANGED, AssistantVisualState::Idle);
        }
        return;
    } else if (event == AssistantEvent::WIFI_LOST) {
        __atomic_store_n(&m_wifi_available_atomic, 0, __ATOMIC_RELAXED);
        transitionTo(AssistantState::Idle);
        return;
    }

    AssistantState current = static_cast<AssistantState>(__atomic_load_n(&m_state_atomic, __ATOMIC_RELAXED));

    switch (current) {
        case AssistantState::Idle:
            if (event == AssistantEvent::WAKE_WORD_DETECTED) {
                if (__atomic_load_n(&m_wifi_available_atomic, __ATOMIC_RELAXED) != 0) {
                    transitionTo(AssistantState::StartingSession);

                    if (GeminiProtocolTask::getInstance().isConnected()) {
                        LOGI_SYSTEM("Persistent WS active: skipping Connecting state.");
                        transitionTo(AssistantState::StreamingUserAudio);
                    } else {
                        transitionTo(AssistantState::Connecting);
                    }
                } else {
                    LOGW_SYSTEM("WakeWord caught but Wi-Fi link reports down. Shifting warning alert to background task.");
                    playAlertAsync(AudioAlertPlayer::playOffline); // Shifted to async worker context
                    PUBLISH_ASSISTANT_EVENT(VISUAL_STATE_CHANGED, AssistantVisualState::Offline);
                    
                    if (m_cooldown_timer) {
                        esp_timer_start_once(m_cooldown_timer, 2ULL * 1000 * 1000); 
                    }
                    __atomic_store_n(&m_state_atomic, static_cast<int>(AssistantState::ErrorCooldown), __ATOMIC_RELAXED);
                }
            }
            break;

        case AssistantState::StartingSession:
            if (event == AssistantEvent::WS_CONNECTED) {
                transitionTo(AssistantState::StreamingUserAudio);
            } else if (event == AssistantEvent::VAD_TIMEOUT) {
                transitionTo(AssistantState::Idle);
            }
            break;

        case AssistantState::Connecting:
            if (event == AssistantEvent::WS_CONNECTED) {
                transitionTo(AssistantState::StreamingUserAudio);
            } else if (event == AssistantEvent::CONNECT_TIMEOUT || event == AssistantEvent::WS_CONNECT_FAILED || event == AssistantEvent::VAD_TIMEOUT) {
                transitionTo(AssistantState::Idle);
            } else if (event == AssistantEvent::QUOTA_EXCEEDED) {
                transitionTo(AssistantState::ErrorCooldown);
            } else if (event == AssistantEvent::TRANSPORT_ERROR || event == AssistantEvent::WS_CLOSED) {
                transitionTo(AssistantState::Idle);
            }
            break;

        case AssistantState::StreamingUserAudio:
            if (event == AssistantEvent::ASSISTANT_AUDIO_STARTED) {
                transitionTo(AssistantState::AssistantSpeaking);
            } else if (event == AssistantEvent::ASSISTANT_TURN_COMPLETE) {
                transitionTo(AssistantState::WaitingForFollowup);
            } else if (event == AssistantEvent::GEMINI_GO_AWAY || event == AssistantEvent::WS_CLOSED) {
                transitionTo(AssistantState::Closing);
            } else if (event == AssistantEvent::QUOTA_EXCEEDED) {
                transitionTo(AssistantState::ErrorCooldown);
            } else if (event == AssistantEvent::VAD_TIMEOUT) {
                transitionTo(AssistantState::Idle);
            }
            break;

        case AssistantState::AssistantSpeaking:
            if (event == AssistantEvent::ASSISTANT_TURN_COMPLETE) {
                transitionTo(AssistantState::WaitingForFollowup);
            } else if (event == AssistantEvent::GEMINI_GO_AWAY || event == AssistantEvent::WS_CLOSED) {
                transitionTo(AssistantState::Closing);
            }
            break;

        case AssistantState::WaitingForFollowup:
            if (event == AssistantEvent::USER_SPEECH_DETECTED) {
                transitionTo(AssistantState::StreamingUserAudio);
            } else if (event == AssistantEvent::ASSISTANT_AUDIO_STARTED) {
                transitionTo(AssistantState::AssistantSpeaking);
            } else if (event == AssistantEvent::VAD_TIMEOUT) {
                transitionTo(AssistantState::Idle);
            } else if (event == AssistantEvent::ASSISTANT_TURN_COMPLETE) {
                // resetIdleTimer();
            } else if (event == AssistantEvent::WS_CLOSED) {
                transitionTo(AssistantState::Idle);
            }
            break;

        default:
            break;
    }
}