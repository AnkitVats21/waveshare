#include "AppController.h"
#include "app/audio/AudioService.h"
#include "app/led/LedService.h"
#include "voice_session/AssistantService.h"
#include "services/time/TimeSyncHelper.h"
#include "services/alarm/AlarmService.h"
#include "services/mcp/McpService.h"

#include "core_sysdb/AppLogger.h"
#include "core_sysdb/AsyncNetLogger.h"
#include "core_sysdb/LogRouter.h"
#include "core_sysdb/thread_config.h"
#include "core_sysdb/EmbeddedSysDb.h"
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
            Mcp::McpService::instance().onWifiConnected();
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
