#include "app/AppController.h"
#include "app/audio/AudioService.h"
#include "app/audio/RtpTransceiver.h"
#include "app/led/LedService.h"
#include "app/assistant/AssistantService.h"
#include "app/mqtt/MqttService.h"
#include "app/input/KeyService.h"
#include "app/gemini_live/GeminiProtocol.h"
#include "app/gemini_live/GeminiAudioPump.h"
#include "common/AppLogger.h"
#include "common/LogRouter.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "hal/Board.h"
#include "hal/input/ExpanderKeyInput.h"
#include "hal/network/WifiService.h"
#include "services/BufferManager.h"
#include "esp_netif.h"
#include "esp_event.h"

extern "C" void app_main(void) {
    // 1. Initialize Foundational Network Stack & Log Routing
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t loop_ret = esp_event_loop_create_default();
    if (loop_ret != ESP_OK && loop_ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(loop_ret);
    }

    LogRouter::getInstance().init();
    LOGI_SYSTEM("Initializing System Application Layer...");

    // 2. Boot SysDb with Kconfig defaults
    EmbeddedSysDb::getInstance().mutate(COMP::SYSTEM | COMP::AUDIO, [](SystemState& s) {
        s.system.wifi_ssid        = CONFIG_WAVESHARE_WIFI_SSID;
        s.system.wifi_password    = CONFIG_WAVESHARE_WIFI_PASSWORD;
        s.system.server_ip        = CONFIG_WAVESHARE_SERVER_IP;
        s.audio.sample_rate       = 16000;
        s.audio.speaker_volume    = 80;
        s.audio.mic_gain_db       = 60.0f;
        s.audio.rtp_tx_port       = CONFIG_WAVESHARE_RTP_TX_PORT;
        s.audio.rtp_rx_port       = CONFIG_WAVESHARE_RTP_RX_PORT;
        s.audio.buffer_size       = 131072;
    });

    // 2.5 Initialize Ring Buffers in PSRAM
    if (!BufferManager::getInstance().initAll()) {
        LOGE_SYSTEM("Fatal: Failed to allocate ring buffers in PSRAM!");
    }

    // 3. Initialize Board Hardware
    Board &board = Board::getInstance();
    board.setSampleRate(EmbeddedSysDb::getInstance().snapshot().audio.sample_rate);
    if (!board.begin()) {
        LOGE_SYSTEM("Fatal: Failed to initialize board hardware!");
    } else {
        LOGI_SYSTEM("Board hardware and NVS ready.");
    }

    // 4. Extract typed HAL references (Dependency Injection)
    AudioHal&        audio_hal = board.getAudio();
    LedStripManager& led_strip = board.getLeds();
    IoExpander&      io_exp    = board.getIoExpanderInstance();

    HardwareAudioHandles handles = {
        .mic_rx_handle     = board.getRxHandle(),
        .speaker_tx_handle = board.getTxHandle(),
        .play_dev          = board.getPlayDev(),
        .record_dev        = board.getRecordDev(),
    };

    // 5. Construct ReactorTask services
    static AudioService         audio_svc(audio_hal, handles);
    static LedService           led_svc(led_strip);
    static AssistantService     assistant_svc;
    static MqttService&         mqtt_svc = MqttService::getInstance();
    // static RtpTransceiver       rtp_trans;
    static GeminiProtocol&      gemini_proto = GeminiProtocol::getInstance();
    (void)gemini_proto; // Suppress unused warning since task auto-spawns on instantiation
    static GeminiAudioPump&     gemini_pump = GeminiAudioPump::getInstance();
    static AppController&       app_ctrl = AppController::getInstance();

    // Start services
    audio_svc.begin();
    assistant_svc.begin();
    mqtt_svc.begin();
    // rtp_trans.begin();
    gemini_pump.start();
    app_ctrl.begin();

    // 6. Initialize Key Input service
    static ExpanderKeyInput key_input(io_exp);
    static KeyService key_svc(key_input);
    key_svc.begin();

    // 6.5 Spawn ReactorTask background threads
    audio_svc.start();
    led_svc.start();
    assistant_svc.start();
    mqtt_svc.start();
    // rtp_trans.start();
    gemini_proto.start();
    app_ctrl.start();
    key_svc.start();

    // 7. Start WiFi service event bridge
    WifiService::Config wifi_cfg = {
        .ssid        = EmbeddedSysDb::getInstance().snapshot().system.wifi_ssid,
        .password    = EmbeddedSysDb::getInstance().snapshot().system.wifi_password,
        .max_retries = 5,
    };
    static WifiService wifi(wifi_cfg);
    wifi.begin();

    LOGI_SYSTEM("System initialization complete. Monitoring system events...");
}

