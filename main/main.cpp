#include "app/AppController.h"
#include "app/audio/AudioService.h"
#include "app/audio/RtpPlayer.h"
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
#include "services/alarm/AlarmService.h"
#include "services/storage/StorageService.h"
#include "services/storage/SysDbSyncReactor.h"
#include "hal/input/ExpanderKeyInput.h"
#include <sstream>
#include "hal/network/WifiService.h"
#include "services/BufferManager.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "common/ParserUtils.h"

#if CONFIG_WAVESHARE_SDCARD_ENABLE
struct StateParseCtx {
    int volume = 80;
    float gain = 60.0f;
    int r = 0, g = 0, b = 0;
};

static void onStatePair(const std::string& key, const std::string& val, void* ctx) {
    auto* p = static_cast<StateParseCtx*>(ctx);
    if (key == "speaker_volume") p->volume = std::stoi(val);
    else if (key == "led_color") sscanf(val.c_str(), "%d,%d,%d", &p->r, &p->g, &p->b);
}

static void loadPersistentState() {
    if (Services::StorageService::getInstance().fileExists("/sdcard/state_sync.txt")) {
        std::string content = Services::StorageService::getInstance().readFile("/sdcard/state_sync.txt");
        if (!content.empty()) {
            StateParseCtx parseCtx;
            Utils::ParserUtils::parseKeyValueStream(content, onStatePair, &parseCtx);
            
            EmbeddedSysDb::getInstance().mutate([parseCtx](SystemState& s) {
                s.audio.speaker_volume = parseCtx.volume;
                s.led.color = { (uint8_t)parseCtx.r, (uint8_t)parseCtx.g, (uint8_t)parseCtx.b };
                s.led.mode = LedMode::SOLID;
            });
            LOGI_SYSTEM("Persistent state loaded from SD card: vol=%d, color=%d,%d,%d",
                        parseCtx.volume, parseCtx.r, parseCtx.g, parseCtx.b);
        }
    }
}
#endif

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
    EmbeddedSysDb::getInstance().mutate(
        [](SystemState& s) {
            strncpy(s.system.server_ip, CONFIG_WAVESHARE_SERVER_IP, sizeof(s.system.server_ip) - 1);
            s.system.server_ip[sizeof(s.system.server_ip) - 1] = '\0';
            s.audio.sample_rate       = 24000;
            s.audio.speaker_volume    = 80;
            s.audio.mic_gain_db       = 60.0f;
            s.audio.rtp_tx_port       = CONFIG_WAVESHARE_RTP_TX_PORT;
            s.audio.rtp_rx_port       = CONFIG_WAVESHARE_RTP_RX_PORT;
            s.audio.buffer_size       = 131072;
        }
    );

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
#if CONFIG_WAVESHARE_SDCARD_ENABLE
        if (board.initSdCard(CONFIG_WAVESHARE_SDCARD_MOUNT_POINT, 5) != ESP_OK) {
            LOGE_SYSTEM("Failed to mount SD card!");
        } else {
            LOGI_SYSTEM("SD Card mounted successfully at %s", CONFIG_WAVESHARE_SDCARD_MOUNT_POINT);
            loadPersistentState();
        }
#endif
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
    static RtpPlayer            rtp_player;
    static GeminiProtocol&      gemini_proto = GeminiProtocol::getInstance();
    (void)gemini_proto; // Suppress unused warning since task auto-spawns on instantiation
    static GeminiAudioPump&     gemini_pump = GeminiAudioPump::getInstance();
    static AppController&       app_ctrl = AppController::getInstance();
    static Services::AlarmService& alarm_svc = Services::AlarmService::getInstance();
    static Services::SysDbSyncReactor& sync_reactor = Services::SysDbSyncReactor::getInstance();

    // Start services
    audio_svc.begin();
    assistant_svc.begin();
    mqtt_svc.begin();
    rtp_player.begin();
    gemini_pump.start();
    app_ctrl.begin();
    alarm_svc.begin();
    sync_reactor.begin();


    // 6. Initialize Key Input service
    static ExpanderKeyInput key_input(io_exp);
    static KeyService key_svc(key_input);
    key_svc.begin();

    // 6.5 Spawn ReactorTask background threads
    audio_svc.start();
    led_svc.start();
    assistant_svc.start();
    mqtt_svc.start();
    rtp_player.start();
    gemini_proto.start();
    app_ctrl.start();
    key_svc.start();
    alarm_svc.start();
    sync_reactor.start();


    // 7. Start WiFi service event bridge
    WifiService::Config wifi_cfg = {
        .ssid        = CONFIG_WAVESHARE_WIFI_SSID,
        .password    = CONFIG_WAVESHARE_WIFI_PASSWORD,
        .max_retries = 5,
    };
    static WifiService wifi(wifi_cfg);
    wifi.begin();

    LOGI_SYSTEM("System initialization complete. Monitoring system events...");
}

