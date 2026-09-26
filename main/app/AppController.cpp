#include "AppController.h"
#include "app/audio/AudioService.h"
#include "gemini_live/GeminiProtocol.h"
#include "gemini_live/gemini_skills_generated.h"
#include "app/led/LedService.h"
#include "gemini_live/AssistantService.h"
#include "gemini_live/MediaCommandHandler.h"
#include "gemini_live/DeviceCommandHandler.h"
#include "services/time/TimeSyncHelper.h"
#include "services/alarm/AlarmService.h"

#include "common/AppLogger.h"
#include "common/AsyncNetLogger.h"
#include "common/LogRouter.h"
#include "common/thread_config.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "hal/Board.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include <string>
#include <cstring>
#include <ArduinoJson.h>

AppController::AppController()
    : ReactorTask({
          "app_ctrl",
          ThreadConfig::StackSize::STACK_NORMAL,
          ThreadConfig::Priority::LOW,
          ThreadConfig::CORE_NETWORK,
          COMP::SYSTEM | COMP::AUDIO | COMP::BLUETOOTH
      })
{}

AppController &AppController::getInstance() {
    static AppController instance;
    return instance;
}

bool AppController::begin() {
    // Register platform delegate for local device commands
    DeviceCommandHandler::setDelegate(this);

    // Register tool-call handler callback with GeminiProtocol
    GeminiProtocol::getInstance().setToolCallHandler(handleGeminiToolCall, this);

    m_wifi_connected = EmbeddedSysDb::getInstance().snapshot().system.wifi_connected;

    LOGI_SYSTEM("AppController initialized.");
    return true;
}

void AppController::onStateChanged(ComponentMask changed, const SystemState& snap) {
    // NOTE: per-field BIT_* values are only unique *within* a component. diffState()
    // OR's them together as (COMP::X | BIT_X::FIELD), so every per-field test must
    // also gate on its COMP:: bit.
    if ((changed & COMP::AUDIO) && (changed & BIT_AUDIO::SPEAKER_VOLUME)) {
        // Speaker volume changed
    }

    if ((changed & COMP::SYSTEM) && (changed & BIT_SYSTEM::WIFI_CONNECTED)) {
        bool wifi_ok = snap.system.wifi_connected;

        if (wifi_ok) {
            // Syncs now if the clock is not set; retries with backoff on failure.
            Services::TimeSyncHelper::instance().onWifiConnected();
        }

        // if (wifi_ok && !m_wifi_connected) {
        //     m_wifi_connected = true;
        //     LOGI_SYSTEM("Network connected. Initializing net logging...");
        //     // AsyncNetLogger::getInstance().startWorker();
        //     LogRouter::getInstance().setNetworkStreamingState(
        //         LogRouter::State::ROUTE_CONSOLE_ONLY);
        // } else if (!wifi_ok && m_wifi_connected) {
        //     m_wifi_connected = false;
        //     LOGW_SYSTEM("Network disconnected. Stopping net logging...");
        //     LogRouter::getInstance().setNetworkStreamingState(
        //         LogRouter::State::ROUTE_CONSOLE_ONLY);
        //     AsyncNetLogger::getInstance().stopWorker();
        // }
    }
}

void AppController::handleGeminiToolCall(const GeminiSkills::DecodedSkillCall& skill_call, void* ctx) {
    auto self = static_cast<AppController*>(ctx);
    if (self) {
        self->executeToolCall(skill_call);
    }
}

void AppController::executeToolCall(const GeminiSkills::DecodedSkillCall& skill_call) {
    JsonDocument response_doc;
    bool is_media_command = false;

    // Try device/local commands first; if not handled, fall back to media command handler
    if (!DeviceCommandHandler::handle(skill_call, response_doc)) {
        if (MediaCommandHandler::handle(skill_call, response_doc)) {
            is_media_command = true;
        } else {
            response_doc["status"] = "error";
            response_doc["message"] = "Unsupported tool skill";
        }
    }

    std::string feedback_string;
    serializeJson(response_doc, feedback_string);

    LOGI_SYSTEM("Uplinking tool response: %s", feedback_string.c_str());
    GeminiProtocol::getInstance().transmitToolResponse(skill_call.call_id, feedback_string.c_str());

    // If it was a media command, set the media_pending_idle flag to trigger immediate session termination once speaking finishes
    if (is_media_command) {
        LOGI_SYSTEM("Media command handled. Setting pending idle flag to bypass VAD delay after speech confirmation.");
        auto& sysdb = EmbeddedSysDb::getInstance();
        sysdb.mutate([](SystemState& s) {
            s.assistant.media_pending_idle = true;
        });
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// IDeviceCommandDelegate Implementation
// ─────────────────────────────────────────────────────────────────────────────

bool AppController::setAlarm(int hour, int minute, const char* tone_file, bool enabled, int& out_alarm_id) {
    auto& svc = Services::AlarmService::getInstance();
    // Same time as an existing daily alarm: update it.
    int target_id = 0;
    Services::AlarmDoc alarm;
    for (const auto& [id, a] : svc.alarms()) {
        if (a.hour == hour && a.minute == minute && !a.at && a.kind == 0) {
            target_id = id;
            alarm = a;
            break;
        }
    }
    alarm.hour = (uint8_t)hour;
    alarm.minute = (uint8_t)minute;
    if (!target_id) alarm.days = Services::AlarmWhen::EVERY_DAY;
    alarm.enabled = enabled;
    alarm.tone = tone_file ? tone_file : "";  // empty = built-in tone
    if (!alarm.tone.empty() && alarm.tone[0] == '/') alarm.tone.clear();

    ESP_LOGI(TAG, "Tool request: alarm %d at %02d:%02d (tone '%s', %s)", target_id, hour, minute,
             alarm.tone.c_str(), enabled ? "enabled" : "disabled");
    out_alarm_id = svc.saveAlarm(target_id, alarm);
    return out_alarm_id != 0;
}

bool AppController::stopActiveAlarm() {
    ESP_LOGI(TAG, "Tool request: Stopping active alarm tone...");
    Services::AlarmService::getInstance().stopActiveAlarm();
    return true;
}

