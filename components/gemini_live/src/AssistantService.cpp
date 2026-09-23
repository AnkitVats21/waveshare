#include "AssistantService.h"
#include "app/audio/AlertPlayer.h"
#include "app/audio/AudioOrchestrator.h"
#include "GeminiProtocol.h"
#include "common/AppLogger.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "common/thread_config.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static auto& sysdb = EmbeddedSysDb::getInstance();

// Non-blocking, thread-safe alert dispatch via AlertPlayer queue
static void playAlertAsync(AlertType type) {
    AlertPlayer::getInstance().playAlert(type);
}

// ─────────────────────────────────────────────────────────────────────────────
// Timer Callback Bridges
// ─────────────────────────────────────────────────────────────────────────────

void AssistantService::connectTimeoutCallback(void* arg) {
    auto svc = static_cast<AssistantService*>(arg);
    svc->handleConnectTimeout();
}

void AssistantService::idleTimeoutCallback(void* arg) {
    auto svc = static_cast<AssistantService*>(arg);
    svc->handleIdleTimeout();
}

void AssistantService::cooldownTimeoutCallback(void* arg) {
    auto svc = static_cast<AssistantService*>(arg);
    svc->handleCooldownElapsed();
}

void AssistantService::handleConnectTimeout() {
    m_connect_timeout_pending = true;
    xTaskNotify(m_task_handle, COMP::ASSISTANT, eSetBits);
}

void AssistantService::handleIdleTimeout() {
    m_idle_timeout_pending = true;
    xTaskNotify(m_task_handle, COMP::ASSISTANT, eSetBits);
}

void AssistantService::handleCooldownElapsed() {
    m_cooldown_elapsed_pending = true;
    xTaskNotify(m_task_handle, COMP::ASSISTANT, eSetBits);
}

// ─────────────────────────────────────────────────────────────────────────────
// Construction & Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

AssistantService::AssistantService()
    : ReactorTask({
          "assistant_svc",
          ThreadConfig::StackSize::STACK_ASSISTANT,
          ThreadConfig::Priority::ASSISTANT,
          ThreadConfig::CORE_NETWORK,
          COMP::SYSTEM | COMP::AUDIO | COMP::ASSISTANT
      })
{}

AssistantService::~AssistantService() {
    if (m_idle_timer) {
        esp_timer_stop(m_idle_timer);
        esp_timer_delete(m_idle_timer);
    }
    if (m_connect_timer) {
        esp_timer_stop(m_connect_timer);
        esp_timer_delete(m_connect_timer);
    }
    if (m_cooldown_timer) {
        esp_timer_stop(m_cooldown_timer);
        esp_timer_delete(m_cooldown_timer);
    }
}

bool AssistantService::begin() {
    // 1. Create ESP-IDF timers
    if (!m_idle_timer) {
        esp_timer_create_args_t idle_args = {};
        idle_args.callback = idleTimeoutCallback;
        idle_args.arg = this;
        idle_args.dispatch_method = ESP_TIMER_TASK;
        idle_args.name = "session_idle";
        esp_timer_create(&idle_args, &m_idle_timer);
    }

    if (!m_connect_timer) {
        esp_timer_create_args_t connect_args = {};
        connect_args.callback = connectTimeoutCallback;
        connect_args.arg = this;
        connect_args.dispatch_method = ESP_TIMER_TASK;
        connect_args.name = "connect_timeout";
        esp_timer_create(&connect_args, &m_connect_timer);
    }

    if (!m_cooldown_timer) {
        esp_timer_create_args_t cooldown_args = {};
        cooldown_args.callback = cooldownTimeoutCallback;
        cooldown_args.arg = this;
        cooldown_args.dispatch_method = ESP_TIMER_TASK;
        cooldown_args.name = "error_cooldown";
        esp_timer_create(&cooldown_args, &m_cooldown_timer);
    }

    // Initialize state to Idle
    m_current_state = AssistantState::Idle;
    auto snap = sysdb.snapshot();
    bool wifi_ok = snap.system.wifi_connected;

    sysdb.mutate([wifi_ok](SystemState& s) {
        s.assistant.session_state = AssistantState::Idle;
        s.assistant.visual_state = wifi_ok ? AssistantVisualState::Idle : AssistantVisualState::Offline;
        s.assistant.connect_requested = false;
    });

    LOGI_SYSTEM("AssistantService operational.");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReactorTask::onStateChanged
// ─────────────────────────────────────────────────────────────────────────────

void AssistantService::onStateChanged(ComponentMask changed, const SystemState& snap) {
    // 1. Process timer events and deferred transitions
    if (m_pending_idle_transition) {
        m_pending_idle_transition = false;
        transitionTo(AssistantState::Idle, &snap);
        return;
    }
    if (m_connect_timeout_pending) {
        m_connect_timeout_pending = false;
        ESP_LOGW(TAG, "Connection timeout elapsed.");
        transitionTo(AssistantState::ErrorCooldown, &snap);
        return;
    }
    if (m_idle_timeout_pending) {
        m_idle_timeout_pending = false;
        ESP_LOGW(TAG, "Idle timeout elapsed.");
        transitionTo(AssistantState::Closing, &snap);
        return;
    }
    if (m_cooldown_elapsed_pending) {
        m_cooldown_elapsed_pending = false;
        ESP_LOGI(TAG, "Error cooldown elapsed.");
        transitionTo(AssistantState::Idle, &snap);
        return;
    }

    // Get snapshot components
    auto session = snap.assistant.session_state;
    auto ws = snap.assistant.ws_state;
    bool wifi_ok = snap.system.wifi_connected;

    // 2. Synchronize current state shadow if external component changed session_state
    if (session != m_current_state) {
        executeTransition(session, snap, true);
    }

    // 3. React to state machine condition triggers
    switch (m_current_state) {
        case AssistantState::Idle: {
            AssistantVisualState targetVis = AssistantVisualState::Idle;
            if (snap.system.network_state == NetworkState::PortalActive) {
                targetVis = AssistantVisualState::Recovering;
            } else if (snap.system.network_state == NetworkState::Connecting) {
                targetVis = AssistantVisualState::Connecting;
            } else if (!wifi_ok) {
                targetVis = AssistantVisualState::Offline;
            }

            if (snap.assistant.visual_state != targetVis) {
                sysdb.mutate([targetVis](SystemState& s) {
                    s.assistant.visual_state = targetVis;
                });
            }
            break;
        }

        case AssistantState::StartingSession:
            if (!wifi_ok) {
                LOGW_SYSTEM("StartingSession: Wi-Fi reported down.");
                playAlertAsync(ALERT_OFFLINE);
                transitionTo(AssistantState::ErrorCooldown, &snap);
            } else {
                transitionTo(AssistantState::Connecting, &snap);
            }
            break;

        case AssistantState::Connecting:
            if (ws == WsState::CONNECTED) {
                transitionTo(AssistantState::StreamingUserAudio, &snap);
            } else if (ws == WsState::DISCONNECTED || ws == WsState::ERROR_STATE) {
                ESP_LOGE(TAG, "Connecting: WebSocket failed or disconnected.");
                transitionTo(AssistantState::Idle, &snap);
            }
            break;

        case AssistantState::StreamingUserAudio:
            if (snap.audio.assistant_speaking) {
                transitionTo(AssistantState::AssistantSpeaking, &snap);
            } else if (ws == WsState::DISCONNECTED || ws == WsState::GOING_AWAY || ws == WsState::ERROR_STATE) {
                ESP_LOGW(TAG, "StreamingUserAudio: WebSocket closed or error.");
                transitionTo(AssistantState::Closing, &snap);
            }
            break;

        case AssistantState::AssistantSpeaking:
            if (!snap.audio.assistant_speaking && !snap.audio.turn_complete_pending) {
                if (snap.assistant.media_pending_idle) {
                    ESP_LOGI(TAG, "Media command was executed and speech playback completed. Transitioning directly to Closing to bypass VAD.");
                    transitionTo(AssistantState::Closing, &snap);
                } else {
                    transitionTo(AssistantState::WaitingForFollowup, &snap);
                }
            } else if (ws == WsState::DISCONNECTED || ws == WsState::GOING_AWAY || ws == WsState::ERROR_STATE) {
                ESP_LOGW(TAG, "AssistantSpeaking: WebSocket closed or error.");
                transitionTo(AssistantState::Closing, &snap);
            }
            break;

        case AssistantState::WaitingForFollowup:
            if (snap.assistant.media_pending_idle) {
                ESP_LOGI(TAG, "Media command was executed. Transitioning directly to Closing from WaitingForFollowup.");
                transitionTo(AssistantState::Closing, &snap);
            } else if (ws == WsState::DISCONNECTED || ws == WsState::GOING_AWAY || ws == WsState::ERROR_STATE) {
                ESP_LOGW(TAG, "WaitingForFollowup: WebSocket lost (state=%d). Returning to Idle.", (int)ws);
                transitionTo(AssistantState::Idle, &snap);
            }
            break;

        default:
            break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// State Machine Transitions
// ─────────────────────────────────────────────────────────────────────────────

static const char* assistantStateToString(AssistantState state) {
    switch (state) {
        case AssistantState::Idle:               return "Idle";
        case AssistantState::StartingSession:    return "StartingSession";
        case AssistantState::Connecting:         return "Connecting";
        case AssistantState::StreamingUserAudio: return "StreamingUserAudio";
        case AssistantState::AssistantSpeaking:  return "AssistantSpeaking";
        case AssistantState::WaitingForFollowup: return "WaitingForFollowup";
        case AssistantState::Closing:            return "Closing";
        case AssistantState::ErrorCooldown:      return "ErrorCooldown";
        default:                                 return "Unknown";
    }
}

void AssistantService::transitionTo(AssistantState newState, const SystemState* snap_ptr) {
    SystemState local_snap;
    if (!snap_ptr) {
        local_snap = sysdb.snapshot();
        snap_ptr = &local_snap;
    }
    executeTransition(newState, *snap_ptr, false);
}

void AssistantService::executeTransition(AssistantState newState, const SystemState& snap, bool is_external_sync) {
    AssistantState oldState = m_current_state;
    if (oldState == newState) {
        return;
    }

    if (is_external_sync) {
        LOGI_SYSTEM("Syncing local state machine from external change: %s ──> %s",
                    assistantStateToString(oldState), assistantStateToString(newState));
    } else {
        LOGI_SYSTEM("Assistant transition request: %s ──> %s",
                    assistantStateToString(oldState), assistantStateToString(newState));
    }

    // 1. Cleanup timers of the old state
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

    // 2. Compute visual state, pipeline mode, and triggers for the new state
    AssistantVisualState visState = AssistantVisualState::Idle;
    PipelineMode pipeMode = PipelineMode::WAKE_IDLE;
    bool sessionActive = false;
    bool micEnabled = false;
    bool connectRequested = false;
    bool trigger_auto_transition_to_idle = false;
    bool wifi_connected = snap.system.wifi_connected;

    switch (newState) {
        case AssistantState::Idle:
            visState = wifi_connected ? AssistantVisualState::Idle : AssistantVisualState::Offline;
            pipeMode = PipelineMode::WAKE_IDLE;
            sessionActive = false;
            micEnabled = false;
            connectRequested = false;
            GeminiProtocol::getInstance().closeConnection();
            break;

        case AssistantState::StartingSession:
            visState = AssistantVisualState::Thinking;
            pipeMode = PipelineMode::WAKE_IDLE;
            sessionActive = false;
            micEnabled = false;
            connectRequested = false;
            break;

        case AssistantState::Connecting:
            visState = AssistantVisualState::Connecting;
            pipeMode = PipelineMode::WAKE_IDLE;
            sessionActive = false;
            micEnabled = false;
            connectRequested = true;
            if (m_connect_timer) {
                esp_timer_start_once(m_connect_timer, CONNECT_TIMEOUT_US);
            }
            break;

        case AssistantState::StreamingUserAudio:
            visState = AssistantVisualState::Listening;
            pipeMode = PipelineMode::GEMINI_LIVE;
            sessionActive = true;
            micEnabled = true;
            connectRequested = false;
            break;

        case AssistantState::AssistantSpeaking:
            visState = AssistantVisualState::Speaking;
            pipeMode = PipelineMode::GEMINI_LIVE;
            sessionActive = true;
            micEnabled = false;
            connectRequested = false;
            break;

        case AssistantState::WaitingForFollowup:
            visState = AssistantVisualState::Thinking;
            pipeMode = PipelineMode::GEMINI_LIVE;
            sessionActive = true;
            micEnabled = false;
            connectRequested = false;
            if (m_idle_timer) {
                esp_timer_start_once(m_idle_timer, SESSION_FOLLOWUP_TIMEOUT_US);
            }
            break;

        case AssistantState::Closing:
            visState = AssistantVisualState::Idle;
            pipeMode = PipelineMode::WAKE_IDLE;
            sessionActive = false;
            micEnabled = false;
            connectRequested = false;
            trigger_auto_transition_to_idle = true;
            break;

        case AssistantState::ErrorCooldown:
            visState = AssistantVisualState::Error;
            pipeMode = PipelineMode::WAKE_IDLE;
            sessionActive = false;
            micEnabled = false;
            connectRequested = false;
            if (m_cooldown_timer) {
                esp_timer_start_once(m_cooldown_timer, COOLDOWN_TIMEOUT_US);
            }
            break;
    }

    // 3. Single atomic SysDb mutation
    sysdb.mutate([newState, visState, pipeMode, sessionActive, micEnabled, connectRequested](SystemState& s) {
        s.assistant.session_state = newState;
        s.assistant.visual_state  = visState;
        s.pipeline.mode           = pipeMode;
        s.audio.session_active    = sessionActive;
        s.audio.mic_enabled       = micEnabled;
        s.assistant.connect_requested = connectRequested;
        if (newState == AssistantState::AssistantSpeaking) {
            s.audio.assistant_speaking = true;
        }
        if (newState == AssistantState::Idle) {
            s.assistant.media_pending_idle = false;
            s.audio.turn_complete_pending  = false;
            s.audio.assistant_speaking     = false;
        }
    });

    m_current_state = newState;

    // 4. Play audio alerts asynchronously
    switch (newState) {
        case AssistantState::StartingSession:
            AudioOrchestrator::getInstance().notifyWakeWordDetected();
            playAlertAsync(ALERT_WAKE_CONFIRM);
            break;
        case AssistantState::StreamingUserAudio:
            playAlertAsync(ALERT_READY_TO_SPEAK);
            break;
        case AssistantState::Closing:
            playAlertAsync(ALERT_SESSION_END);
            break;
        case AssistantState::ErrorCooldown:
            playAlertAsync(ALERT_ERROR);
            break;
        default:
            break;
    }

    // Defer Closing→Idle via task notification to avoid recursive executeTransition()
    if (trigger_auto_transition_to_idle) {
        m_pending_idle_transition = true;
        xTaskNotify(m_task_handle, COMP::ASSISTANT, eSetBits);
    }
}
