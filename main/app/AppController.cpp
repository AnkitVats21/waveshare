#include "AppController.h"
#include "app/audio/AudioPipelineManager.h"
#include "app/audio/AudioService.h"
#include "app/gemini_live/GeminiProtocol.h"
#include "app/gemini_live/gemini_skills_generated.h"
#include "app/led/LedService.h"
#include "app/assistant/AssistantService.h"
#include "app/mqtt/MqttService.h"
#include "app/assistant/MediaCommandHandler.h"
#include "app/assistant/DeviceCommandHandler.h"
#include "services/time/TimeSyncHelper.h"

#include "common/AppLogger.h"
#include "common/AsyncNetLogger.h"
#include "common/LogRouter.h"
#include "common/thread_config.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "hal/Board.h"
#include "esp_heap_caps.h"
#include <string>
#include <cstring>
#include <ArduinoJson.h>

AppController::AppController()
    : ReactorTask({
          "app_ctrl",
          ThreadConfig::StackSize::STACK_SMALL,
          ThreadConfig::Priority::LOW,
          ThreadConfig::CORE_NETWORK,
          COMP::SYSTEM
      })
{}

AppController &AppController::getInstance() {
    static AppController instance;
    return instance;
}

bool AppController::begin() {
    // Register tool-call handler callback with GeminiProtocol
    GeminiProtocol::getInstance().setToolCallHandler(handleGeminiToolCall, this);

    m_wifi_connected = EmbeddedSysDb::getInstance().snapshot().system.wifi_connected;

    LOGI_SYSTEM("AppController initialized.");
    return true;
}

void AppController::onStateChanged(ComponentMask changed, const SystemState& snap) {
    if (changed & BIT_SYSTEM::WIFI_CONNECTED) {
        bool wifi_ok = snap.system.wifi_connected;

        if (wifi_ok && !m_time_synced) {
            m_time_synced = true;
            LOGI_SYSTEM("Wi-Fi connected. Spawning background NTP synchronization task...");
            xTaskCreate([](void* arg) {
                Services::TimeSyncHelper::synchronizeTimeAndCleanup();
                vTaskDelete(NULL);
            }, "ntp_sync", 3072, NULL, 4, NULL);
        }
        
        // if (wifi_ok && !m_wifi_connected) {
        //     m_wifi_connected = true;
        //     LOGI_SYSTEM("Network connected. Initializing net logging...");
        //     // AsyncNetLogger::getInstance().init(snap.system.server_ip, 5006);
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
    bool is_mpv_command = false;

    // Try device/local commands first; if not handled, fall back to MPV command handler
    if (!DeviceCommandHandler::handle(skill_call, response_doc)) {
        if (MediaCommandHandler::handle(skill_call, response_doc)) {
            is_mpv_command = true;
        } else {
            response_doc["status"] = "error";
            response_doc["message"] = "Unsupported tool skill";
        }
    }

    std::string feedback_string;
    serializeJson(response_doc, feedback_string);

    LOGI_SYSTEM("Uplinking tool response: %s", feedback_string.c_str());
    GeminiProtocol::getInstance().transmitToolResponse(skill_call.call_id, feedback_string.c_str());

    // If it was an MPV command, set the mpv_pending_idle flag to trigger immediate session termination once speaking finishes
    if (is_mpv_command) {
        LOGI_SYSTEM("MPV command handled. Setting pending idle flag to bypass VAD delay after speech confirmation.");
        auto& sysdb = EmbeddedSysDb::getInstance();
        sysdb.mutate([](SystemState& s) {
            s.assistant.mpv_pending_idle = true;
        });
    }
}
