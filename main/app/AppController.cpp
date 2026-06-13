#include "AppController.h"
#include "app/audio/AudioPipelineManager.h"
#include "app/audio/AudioService.h"
#include "app/gemini_live/GeminiProtocol.h"
#include "app/gemini_live/gemini_skills_generated.h"
#include "app/led/LedService.h"
#include "app/assistant/AssistantService.h"
#include "app/mqtt/MqttService.h"
#include "app/assistant/MpvCommandHandler.h"

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
    // Handled directly in run loop via xTaskNotifyWait
}

void AppController::run() {
    LOGI_SYSTEM("AppController supervisor thread active.");

    while (m_running) {
        uint32_t changed_bits = 0;
        BaseType_t notified = xTaskNotifyWait(0, 0xFFFFFFFF, &changed_bits, pdMS_TO_TICKS(100));
        if (!m_running) break;

        if (notified == pdTRUE && changed_bits > 0) {
            m_last_changed = changed_bits;
            SystemState snap = EmbeddedSysDb::getInstance().snapshot();
            onStateChanged(m_last_changed, snap);
        }

        auto snap = EmbeddedSysDb::getInstance().snapshot();
        bool wifi_ok = snap.system.wifi_connected;

        if (wifi_ok && !m_wifi_connected) {
            m_wifi_connected = true;
            LOGI_SYSTEM("Network connected. Initializing net logging...");
            AsyncNetLogger::getInstance().init(snap.system.server_ip, 5006);
            AsyncNetLogger::getInstance().startWorker();
            LogRouter::getInstance().setNetworkStreamingState(
                LogRouter::State::ROUTE_CONSOLE_AND_NETWORK);
        } else if (!wifi_ok && m_wifi_connected) {
            m_wifi_connected = false;
            LOGW_SYSTEM("Network disconnected. Stopping net logging...");
            LogRouter::getInstance().setNetworkStreamingState(
                LogRouter::State::ROUTE_CONSOLE_ONLY);
            AsyncNetLogger::getInstance().stopWorker();
        }
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

    if (!MpvCommandHandler::handle(skill_call, response_doc)) {
        response_doc["status"] = "error";
        response_doc["message"] = "Unsupported tool skill";
    }

    std::string feedback_string;
    serializeJson(response_doc, feedback_string);

    LOGI_SYSTEM("Uplinking tool response: %s", feedback_string.c_str());
    GeminiProtocol::getInstance().transmitToolResponse(skill_call.call_id, feedback_string.c_str());
}
