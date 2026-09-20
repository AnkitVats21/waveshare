#include "app/AppController.h"
#include "app/audio/AudioService.h"
#include "app/audio/AudioOrchestrator.h"
#include "app/audio/AlertPlayer.h"
#include "app/led/LedService.h"
#include "gemini_live/AssistantService.h"
#include "app/input/KeyService.h"
#include "gemini_live/GeminiProtocol.h"
#include "gemini_live/GeminiAudioPump.h"
#include "app/media_player/NexusPlayer.h"
#include "app/media_player/MusicPlaybackService.h"
#include "common/AppLogger.h"
#include "common/LogRouter.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "hal/Board.h"
// #include "services/alarm/AlarmService.h"
#include "services/storage/StorageService.h"
#include "services/storage/SysDbSyncReactor.h"
#include "services/storage/AlertFileDecoder.h"
#include "hal/input/ExpanderKeyInput.h"
#include "services/network/WifiService.h"
#include "services/network/HttpFileServerService.h"
#include "services/BufferManager.h"
#if CONFIG_DISPLAY_ENABLE
#include "hal/display/LcdManager.h"
#endif

#include "esp_netif.h"
#include "esp_event.h"
#include "esp_ota_ops.h"

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
            s.audio.sample_rate       = LOCAL_SAMPLE_RATE;
            s.audio.speaker_volume    = 80;
            s.audio.mic_gain_db       = 60.0f;
            s.audio.rtp_tx_port       = CONFIG_WAVESHARE_RTP_TX_PORT;
            s.audio.rtp_rx_port       = CONFIG_WAVESHARE_RTP_RX_PORT;
            s.audio.buffer_size       = 131072;
            s.audio.record_all_mic_channels = true;
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
        if (board.initSdCard(CONFIG_WAVESHARE_SDCARD_MOUNT_POINT, 8) != ESP_OK) {
            LOGE_SYSTEM("Failed to mount SD card!");
        } else {
            LOGI_SYSTEM("SD Card mounted successfully at %s", CONFIG_WAVESHARE_SDCARD_MOUNT_POINT);
            Services::SysDbSyncReactor::getInstance().loadPersistentState();
        }
#endif
    }

    // 3.5 Start WiFi service early to secure internal DMA buffers before tasks allocate stacks
    WifiService::Config wifi_cfg = {
        .ssid        = CONFIG_WAVESHARE_WIFI_SSID,
        .password    = CONFIG_WAVESHARE_WIFI_PASSWORD,
        .max_retries = 5,
    };
    static WifiService wifi(wifi_cfg);
    wifi.begin();

#if CONFIG_DISPLAY_ENABLE
    LcdManager::getInstance().begin();
#endif

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
    // Companion (ESP32-WROOM) audio transport is no longer wired over UART/I2S here —
    // it moves to a Wi-Fi/WebSocket bridge (DB A, opusBridge). See .agent/opus-bridge-part1-host-design.md.
    static AudioService         audio_svc(audio_hal, handles);
    static LedService           led_svc(led_strip);
    static AssistantService     assistant_svc;
    static GeminiProtocol&      gemini_proto = GeminiProtocol::getInstance();
    (void)gemini_proto; // Suppress unused warning since task auto-spawns on instantiation
    static GeminiAudioPump&     gemini_pump = GeminiAudioPump::getInstance();
    static AppController&       app_ctrl = AppController::getInstance();
    static Services::SysDbSyncReactor& sync_reactor = Services::SysDbSyncReactor::getInstance();
#if CONFIG_WAVESHARE_HTTP_FILE_SERVER_ENABLE
    static Services::HttpFileServerService& http_server = Services::HttpFileServerService::getInstance();
#endif

    // Start services
    audio_svc.begin();
    AudioOrchestrator::getInstance().begin();
    AlertPlayer::getInstance().setFileDecoder(&AlertFileDecoder::getInstance());
    AlertPlayer::getInstance().begin();
    assistant_svc.begin();
    GeminiProtocol::getInstance().setStorageService(&Services::StorageService::getInstance());
    NexusPlayer::getInstance().setStorageService(&Services::StorageService::getInstance());
    NexusPlayer::getInstance().begin();
    MusicPlaybackService::getInstance().begin();
    gemini_pump.start();
    app_ctrl.begin();
    sync_reactor.begin();
#if CONFIG_WAVESHARE_HTTP_FILE_SERVER_ENABLE
    http_server.begin();
#endif

    // 6. Initialize Key Input service
    static ExpanderKeyInput key_input(io_exp);
    static KeyService key_svc(key_input);
    key_svc.begin();

    // 6.5 Spawn ReactorTask background threads
    wifi.start();
    audio_svc.start();
    AlertPlayer::getInstance().start();
    led_svc.start();
    assistant_svc.start();
    gemini_proto.start();
    app_ctrl.start();
    key_svc.start();
    sync_reactor.start();
    NexusPlayer::getInstance().start();
#if CONFIG_WAVESHARE_HTTP_FILE_SERVER_ENABLE
    http_server.start();
#endif

    // 7. Confirm healthy boot for OTA rollback protection
    esp_ota_mark_app_valid_cancel_rollback();

    LOGI_SYSTEM("System initialization complete. Monitoring system events...");
}

