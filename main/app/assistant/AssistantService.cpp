#include "AssistantService.h"
#include "app/audio/AudioAlertPlayer.h"
#include "app/mqtt/MqttService.h"
#include "common/AppLogger.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "common/thread_config.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
    
static auto& sysdb = EmbeddedSysDb::getInstance();

// Helper: play an audio alert on a tiny fire-and-forget FreeRTOS task.
static void playAlertTask(void* arg) {
    auto fn = reinterpret_cast<void(*)()>(arg);
    if (fn) {
        fn();
    }
    vTaskDelete(nullptr);
}

static void playAlertAsync(void (*fn)()) {
    xTaskCreatePinnedToCore(playAlertTask, "alert_async", 3072, (void*)fn, ThreadConfig::Priority::AUDIO_ALERT, nullptr, 1);
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
          ThreadConfig::StackSize::STACK_NORMAL,
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
    // 1. Process timer events
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

    // 2. Synchronize current state shadow
    if (session != m_current_state) {
        handleStateTransition(m_current_state, session, snap);
    }

    // 3. React to state machine condition triggers
    switch (m_current_state) {
        case AssistantState::Idle:
            // If Wi-Fi link went down, update the visual state to Offline
            if (!wifi_ok && snap.assistant.visual_state != AssistantVisualState::Offline) {
                sysdb.mutate([](SystemState& s) {
                    s.assistant.visual_state = AssistantVisualState::Offline;
                });
            } else if (wifi_ok && snap.assistant.visual_state == AssistantVisualState::Offline) {
                sysdb.mutate([](SystemState& s) {
                    s.assistant.visual_state = AssistantVisualState::Idle;
                });
            }
            break;

        case AssistantState::StartingSession:
            if (!wifi_ok) {
                LOGW_SYSTEM("StartingSession: Wi-Fi reported down.");
                playAlertAsync(AudioAlertPlayer::playOffline);
                transitionTo(AssistantState::ErrorCooldown, &snap);
            } else {
                if (ws == WsState::CONNECTED) {
                    LOGI_SYSTEM("StartingSession: Persistent connection active. Skipping Connecting.");
                    transitionTo(AssistantState::StreamingUserAudio, &snap);
                } else {
                    transitionTo(AssistantState::Connecting, &snap);
                }
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
            if (snap.audio.turn_complete_pending || !snap.audio.assistant_speaking) {
                if (snap.assistant.mpv_pending_idle) {
                    if (!snap.audio.turn_complete_pending) {
                        ESP_LOGI(TAG, "MPV command was executed and speech playback completed. Transitioning directly to Closing to bypass VAD.");
                        transitionTo(AssistantState::Closing, &snap);
                    }
                } else {
                    transitionTo(AssistantState::WaitingForFollowup, &snap);
                }
            } else if (ws == WsState::DISCONNECTED || ws == WsState::GOING_AWAY || ws == WsState::ERROR_STATE) {
                ESP_LOGW(TAG, "AssistantSpeaking: WebSocket closed or error.");
                transitionTo(AssistantState::Closing, &snap);
            }
            break;

        case AssistantState::WaitingForFollowup:
            if (snap.assistant.mpv_pending_idle) {
                ESP_LOGI(TAG, "MPV command was executed. Transitioning directly to Closing from WaitingForFollowup.");
                transitionTo(AssistantState::Closing, &snap);
            } else if (ws == WsState::DISCONNECTED) {
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
    const SystemState& snap = *snap_ptr;
    AssistantState oldState = snap.assistant.session_state;
    if (oldState == newState) {
        return;
    }

    LOGI_SYSTEM("Assistant transition request: %s ──> %s", assistantStateToString(oldState), assistantStateToString(newState));

    publishMusicCommand(oldState, newState);

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
    bool wifi_connected = snap.system.wifi_connected;

    switch (newState) {
        case AssistantState::Idle:
            visState = wifi_connected ? AssistantVisualState::Idle : AssistantVisualState::Offline;
            sysdb.mutate([](SystemState& s) {
                s.pipeline.mode = PipelineMode::WAKE_IDLE;
                s.audio.session_active = false;
                s.assistant.mpv_pending_idle = false;
            });
            break;

        case AssistantState::StartingSession:
            visState = AssistantVisualState::Thinking;
            break;

        case AssistantState::Connecting:
            visState = AssistantVisualState::Connecting;
            sysdb.mutate([](SystemState& s) {
                s.assistant.connect_requested = true;
            });
            if (m_connect_timer) {
                esp_timer_start_once(m_connect_timer, 10ULL * 1000 * 1000); // 10s timeout
            }
            break;

        case AssistantState::StreamingUserAudio:
            visState = AssistantVisualState::Listening;
            sysdb.mutate([](SystemState& s) {
                s.pipeline.mode = PipelineMode::GEMINI_LIVE;
                s.audio.session_active = true;
                s.audio.mic_enabled = true;
            });
            break;

        case AssistantState::AssistantSpeaking:
            visState = AssistantVisualState::Speaking;
            sysdb.mutate([](SystemState& s) {
                s.audio.assistant_speaking = true;
                s.audio.mic_enabled = false;
            });
            break;

        case AssistantState::WaitingForFollowup:
            visState = AssistantVisualState::Thinking;
            if (m_idle_timer) {
                esp_timer_start_once(m_idle_timer, 60ULL * 1000 * 1000); // 60s window
            }
            break;

        case AssistantState::Closing:
            sysdb.mutate([](SystemState& s) {
                s.assistant.connect_requested = false;
                s.pipeline.mode = PipelineMode::WAKE_IDLE;
                s.audio.session_active = false;
            });
            trigger_auto_transition_to_idle = true; 
            break;

        case AssistantState::ErrorCooldown:
            visState = AssistantVisualState::Error;
            sysdb.mutate([](SystemState& s) {
                s.assistant.connect_requested = false;
                s.pipeline.mode = PipelineMode::WAKE_IDLE;
                s.audio.session_active = false;
            });
            if (m_cooldown_timer) {
                esp_timer_start_once(m_cooldown_timer, 5ULL * 1000 * 1000); // 5s cooldown
            }
            break;
    }

    // Apply session and visual states to SysDb
    sysdb.mutate([newState, visState](SystemState& s) {
        s.assistant.session_state = newState;
        s.assistant.visual_state = visState;
        if (newState == AssistantState::Idle) {
            s.assistant.connect_requested = false;
        }
    });

    // 3. Play audio alerts asynchronously
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

    // Handle immediate automatic transitions
    if (trigger_auto_transition_to_idle) {
        transitionTo(AssistantState::Idle);
    }
}

void AssistantService::handleStateTransition(AssistantState oldState, AssistantState newState, const SystemState& snap) {
    m_current_state = newState;
    LOGI_SYSTEM("Syncing local state machine from external change: %s ──> %s", assistantStateToString(oldState), assistantStateToString(newState));

    publishMusicCommand(oldState, newState);

    // Sync timers and internal variables
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

    // Start appropriate timers or sync database if updated externally
    switch (newState) {
        case AssistantState::Idle:
            sysdb.mutate([](SystemState& s) {
                s.pipeline.mode = PipelineMode::WAKE_IDLE;
                s.audio.session_active = false;
                s.assistant.connect_requested = false;
                s.assistant.mpv_pending_idle = false;
            });
            break;
        case AssistantState::Connecting:
            if (m_connect_timer) {
                esp_timer_start_once(m_connect_timer, 10ULL * 1000 * 1000);
            }
            break;
        case AssistantState::WaitingForFollowup:
            if (m_idle_timer) {
                esp_timer_start_once(m_idle_timer, 30ULL * 1000 * 1000);
            }
            break;
        case AssistantState::Closing:
            sysdb.mutate([](SystemState& s) {
                s.assistant.connect_requested = false;
                s.pipeline.mode = PipelineMode::WAKE_IDLE;
                s.audio.session_active = false;
            });
            break;
        case AssistantState::ErrorCooldown:
            sysdb.mutate([](SystemState& s) {
                s.assistant.connect_requested = false;
                s.pipeline.mode = PipelineMode::WAKE_IDLE;
                s.audio.session_active = false;
            });
            if (m_cooldown_timer) {
                esp_timer_start_once(m_cooldown_timer, 5ULL * 1000 * 1000);
            }
            break;
        default:
            break;
    }
}



void AssistantService::publishMusicCommand(AssistantState oldState, AssistantState newState) {
    if (newState == AssistantState::StartingSession) {
        MqttService::getInstance().publish("mpv/command", "{\"cmd\":\"assistant_pause\"}");
    }
    if (oldState != AssistantState::Idle &&
        (newState == AssistantState::Idle || newState == AssistantState::Closing || newState == AssistantState::ErrorCooldown)) {
        MqttService::getInstance().publish("mpv/command", "{\"cmd\":\"assistant_play\"}");
    }
}
