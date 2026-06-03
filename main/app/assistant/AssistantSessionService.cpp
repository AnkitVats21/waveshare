#include "AssistantSessionService.h"
#include "app/event/EventBus.h"
#include "app/audio/AudioAlertPlayer.h"
#include "common/AppLogger.h"
#include <cstring>

AssistantSessionService::AssistantSessionService() : IService("AssistantSessionService") {
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

    subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::WAKE_WORD_DETECTED);
    subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::WIFI_AVAILABLE);
    subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::WIFI_LOST);
    subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::USER_SPEECH_DETECTED);
    subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::VAD_TIMEOUT);
    subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::USER_INTERRUPTED);
    subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::WS_CONNECTED);
    subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::WS_CONNECT_FAILED);
    subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::WS_CLOSED);
    subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::GEMINI_GO_AWAY);
    subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::ASSISTANT_AUDIO_STARTED);
    subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::ASSISTANT_TURN_COMPLETE);
    subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::QUOTA_EXCEEDED);
    subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::SERVER_ERROR);
    subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::TRANSPORT_ERROR);
    subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::CONNECT_TIMEOUT);
    subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::SESSION_IDLE_TIMEOUT);
    subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::COOLDOWN_ELAPSED);

    m_state.store(AssistantState::Idle, std::memory_order_relaxed);
    EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::VISUAL_STATE_CHANGED,
                                    AssistantVisualState::Offline);
    return true;
}

void AssistantSessionService::onStop() {
    LOGI_SYSTEM("Stopping AssistantSessionService...");
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
    AssistantState oldState = m_state.exchange(newState);
    if (oldState == newState) {
        return;
    }

    LOGI_SYSTEM("State transition: %s -> %s", getStateName(oldState), getStateName(newState));

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
    switch (newState) {
        case AssistantState::Idle:
            visState = m_wifi_available ? AssistantVisualState::Idle : AssistantVisualState::Offline;
            EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::AUDIO_RETURN_TO_WAKE_MODE_16K, 0);
            break;

        case AssistantState::StartingSession:
            visState = AssistantVisualState::Thinking;
            AudioAlertPlayer::playWakeConfirm();   // ← audio: "heard you, connecting"
            break;

        case AssistantState::Connecting:
            visState = AssistantVisualState::Connecting;
            EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::TRANSPORT_CONNECT, 0);
            if (m_connect_timer) {
                esp_timer_start_once(m_connect_timer, 10ULL * 1000 * 1000); // 10s connect timeout
            }
            break;

        case AssistantState::StreamingUserAudio:
            visState = AssistantVisualState::Listening;
            AudioAlertPlayer::playReadyToSpeak();  // ← audio: "mic is hot, speak now"
            EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::AUDIO_ENTER_CONVERSATION_MODE, 0);
            EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::TRANSPORT_SEND_BUFFERED_AUDIO, 0);
            EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::TRANSPORT_SEND_LIVE_AUDIO, 0);
            break;

        case AssistantState::AssistantSpeaking:
            visState = AssistantVisualState::Speaking;
            EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::AUDIO_ENTER_PLAYBACK_MODE_24K, 0);
            EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::AUDIO_SUSPEND_MIC_STREAMING, 0);
            break;

        case AssistantState::WaitingForFollowup:
            visState = AssistantVisualState::Thinking;
            if (m_idle_timer) {
                esp_timer_start_once(m_idle_timer, 10ULL * 1000 * 1000); // 10s idle followup timeout
            }
            break;

        case AssistantState::Closing:
            AudioAlertPlayer::playSessionEnd();    // ← audio: "session ending"
            EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::TRANSPORT_CLOSE, 0);
            EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::AUDIO_RETURN_TO_WAKE_MODE_16K, 0);
            // Auto-transition back to Idle
            transitionTo(AssistantState::Idle);
            break;

        case AssistantState::ErrorCooldown:
            visState = AssistantVisualState::Error;
            AudioAlertPlayer::playError();         // ← audio: "something went wrong"
            EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::TRANSPORT_CLOSE, 0);
            EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::AUDIO_RETURN_TO_WAKE_MODE_16K, 0);
            if (m_cooldown_timer) {
                esp_timer_start_once(m_cooldown_timer, 5ULL * 1000 * 1000); // 5s error cooldown
            }
            break;
    }

    EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::VISUAL_STATE_CHANGED, visState);
}

void AssistantSessionService::handleAssistantEvent(AssistantEvent event, void* /*data*/) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Wi-Fi updates apply globally across all states
    if (event == AssistantEvent::WIFI_AVAILABLE) {
        m_wifi_available = true;
        if (m_state.load(std::memory_order_relaxed) == AssistantState::Idle) {
            EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::VISUAL_STATE_CHANGED, AssistantVisualState::Idle);
        }
        return;
    } else if (event == AssistantEvent::WIFI_LOST) {
        m_wifi_available = false;
        transitionTo(AssistantState::Idle);
        return;
    }

    AssistantState current = m_state.load(std::memory_order_relaxed);

    switch (current) {
        case AssistantState::Idle:
            if (event == AssistantEvent::WAKE_WORD_DETECTED) {
                if (m_wifi_available) {
                    transitionTo(AssistantState::StartingSession);
                    transitionTo(AssistantState::Connecting);
                } else {
                    LOGW_SYSTEM("WakeWord detected but Wi-Fi offline. Flashing Offline.");
                    AudioAlertPlayer::playOffline(); // ← audio: "no network"
                    EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::VISUAL_STATE_CHANGED, AssistantVisualState::Offline);
                    // Reset to offline warning, wait a bit, then return to normal
                    if (m_cooldown_timer) {
                        esp_timer_start_once(m_cooldown_timer, 2ULL * 1000 * 1000); // reuse cooldown timer for offline flash
                    }
                    m_state.store(AssistantState::ErrorCooldown, std::memory_order_relaxed);
                }
            }
            break;

        case AssistantState::StartingSession:
            break;

        case AssistantState::Connecting:
            if (event == AssistantEvent::WS_CONNECTED) {
                transitionTo(AssistantState::StreamingUserAudio);
            } else if (event == AssistantEvent::CONNECT_TIMEOUT || event == AssistantEvent::WS_CONNECT_FAILED) {
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
            } else if (event == AssistantEvent::SESSION_IDLE_TIMEOUT) {
                transitionTo(AssistantState::Closing);
            } else if (event == AssistantEvent::GEMINI_GO_AWAY || event == AssistantEvent::WS_CLOSED) {
                transitionTo(AssistantState::Closing);
            } else if (event == AssistantEvent::QUOTA_EXCEEDED) {
                transitionTo(AssistantState::ErrorCooldown);
            } else if (event == AssistantEvent::VAD_TIMEOUT) {
                // Suspends mic capture while we wait for response
                EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::AUDIO_SUSPEND_MIC_STREAMING, 0);
            }
            break;

        case AssistantState::AssistantSpeaking:
            if (event == AssistantEvent::ASSISTANT_TURN_COMPLETE) {
                transitionTo(AssistantState::WaitingForFollowup);
            } else if (event == AssistantEvent::USER_SPEECH_DETECTED || event == AssistantEvent::USER_INTERRUPTED) {
                // Barge-in: flush active playback immediately, resume mic capture, stream barge-in
                EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::AUDIO_FLUSH_PLAYBACK, 0);
                transitionTo(AssistantState::StreamingUserAudio);
            } else if (event == AssistantEvent::GEMINI_GO_AWAY || event == AssistantEvent::WS_CLOSED) {
                transitionTo(AssistantState::Closing);
            }
            break;

        case AssistantState::WaitingForFollowup:
            if (event == AssistantEvent::USER_SPEECH_DETECTED) {
                transitionTo(AssistantState::StreamingUserAudio);
            } else if (event == AssistantEvent::SESSION_IDLE_TIMEOUT) {
                transitionTo(AssistantState::Closing);
            } else if (event == AssistantEvent::WS_CLOSED) {
                transitionTo(AssistantState::Idle);
            }
            break;

        case AssistantState::Closing:
            // Handled via auto-transition in transitionTo
            break;

        case AssistantState::ErrorCooldown:
            if (event == AssistantEvent::COOLDOWN_ELAPSED || event == AssistantEvent::SESSION_IDLE_TIMEOUT) {
                transitionTo(AssistantState::Idle);
            }
            break;
    }
}
