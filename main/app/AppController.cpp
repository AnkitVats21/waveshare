#include "AppController.h"
#include "app/audio/AudioService.h"
#include "app/gemini_live/GeminiProtocol.h"
#include "app/gemini_live/gemini_skills_generated.h"
#include "app/led/LedService.h"
#include "app/assistant/AssistantService.h"
#include "app/assistant/MediaCommandHandler.h"
#include "app/assistant/DeviceCommandHandler.h"
#include "services/time/TimeSyncHelper.h"

#include "common/AppLogger.h"
#include "common/AsyncNetLogger.h"
#include "common/LogRouter.h"
#include "common/thread_config.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "hal/Board.h"
#include "hal/companion/BtPlayerI2s.h"
#include "hal/companion/BtPlayerUart.h"
#include "esp_heap_caps.h"
#include <string>
#include <cstring>
#include <ArduinoJson.h>

AppController::AppController()
    : ReactorTask({
          "app_ctrl",
          ThreadConfig::StackSize::STACK_NORMAL,
          ThreadConfig::Priority::LOW,
          ThreadConfig::CORE_NETWORK,
          COMP::SYSTEM | COMP::AUDIO | COMP::BT_COMPANION
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

#if CONFIG_BT_COMPANION_ENABLE
    initBtCompanion();
#endif

    LOGI_SYSTEM("AppController initialized.");
    return true;
}

void AppController::initBtCompanion() {
#if CONFIG_BT_COMPANION_ENABLE
    LOGI_SYSTEM("Initializing ESP32-WROOM Bluetooth Companion HAL...");

    // 1. Initialize Continuous I2S Master output
    btplayer::BtPlayerI2s::Config i2s_cfg;
    i2s_cfg.port        = CONFIG_BT_COMPANION_I2S_PORT;
    i2s_cfg.bclk_pin    = CONFIG_BT_COMPANION_I2S_BCLK_PIN;
    i2s_cfg.ws_pin      = CONFIG_BT_COMPANION_I2S_WS_PIN;
    i2s_cfg.dout_pin    = CONFIG_BT_COMPANION_I2S_DOUT_PIN;
    i2s_cfg.sample_rate = COMPANION_SAMPLE_RATE;
    esp_err_t err = btplayer::BtPlayerI2s::getInstance().init(i2s_cfg);
    if (err != ESP_OK) {
        LOGE_SYSTEM("Failed to initialize BtPlayerI2s: %s", esp_err_to_name(err));
    }

    // 2. Configure UART Control channel
    btplayer::BtPlayerUart::Config uart_cfg;
    uart_cfg.uart_num = CONFIG_BT_COMPANION_UART_PORT;
    uart_cfg.tx_pin   = CONFIG_BT_COMPANION_UART_TX_PIN;
    uart_cfg.rx_pin   = CONFIG_BT_COMPANION_UART_RX_PIN;
    uart_cfg.baudrate = CONFIG_BT_COMPANION_UART_BAUDRATE;

    auto& bt_uart = btplayer::BtPlayerUart::getInstance();

    bt_uart.setOnReady([](uint16_t fw_version) {
        LOGI_SYSTEM("Companion board ready (FW 0x%04X). Synchronizing volume and connecting BT...", fw_version);
        auto& uart = btplayer::BtPlayerUart::getInstance();
        int vol = EmbeddedSysDb::getInstance().snapshot().audio.speaker_volume;
        uart.setVolume(static_cast<uint8_t>(vol));

        const char* target_speaker = CONFIG_BT_COMPANION_TARGET_SPEAKER;
        if (target_speaker != nullptr && std::strlen(target_speaker) > 0) {
            uart.connectBt(target_speaker);
        } else {
            uart.connectBt(nullptr);
        }
    });

    bt_uart.setOnBtStatus([](bool connected, const std::string& name) {
        if (connected) {
            LOGI_SYSTEM("Companion connected to Bluetooth speaker: \"%s\"", name.c_str());
        } else {
            LOGW_SYSTEM("Companion disconnected from Bluetooth speaker. Audio fallback active.");
        }
    });

    if (!bt_uart.init(uart_cfg)) {
        LOGE_SYSTEM("Failed to initialize BtPlayerUart!");
    } else {
        bt_uart.sendPing();
        const char* target_speaker = CONFIG_BT_COMPANION_TARGET_SPEAKER;
        if (target_speaker != nullptr && std::strlen(target_speaker) > 0) {
            bt_uart.connectBt(target_speaker);
        } else {
            bt_uart.connectBt(nullptr);
        }
    }
#endif
}

void AppController::onStateChanged(ComponentMask changed, const SystemState& snap) {
    // NOTE: per-field BIT_* values are only unique *within* a component. diffState()
    // OR's them together as (COMP::X | BIT_X::FIELD), so every per-field test must
    // also gate on its COMP:: bit — otherwise e.g. a COMP::BT_COMPANION STATUS
    // frame (BIT_BT_COMPANION::STATUS == 1<<2) would masquerade as a
    // BIT_AUDIO::SPEAKER_VOLUME change (also 1<<2) and spuriously re-send SET_VOLUME.
    if ((changed & COMP::AUDIO) && (changed & BIT_AUDIO::SPEAKER_VOLUME)) {
#if CONFIG_BT_COMPANION_ENABLE
        auto& bt_uart = btplayer::BtPlayerUart::getInstance();
        if (bt_uart.isInitialized()) {
            bt_uart.setVolume(static_cast<uint8_t>(snap.audio.speaker_volume));
        }
#endif
    }

    if ((changed & COMP::SYSTEM) && (changed & BIT_SYSTEM::WIFI_CONNECTED)) {
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
