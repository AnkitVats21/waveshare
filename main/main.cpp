#include "app/AppController.h"
#include "app/event/EventBus.h"
#include "common/AppLogger.h"
#include "common/LogRouter.h"
#include "common/app_types.h"
#include "hal/Board.h"
#include "hal/network/WifiManager.h"
#include "hal/input/ExpanderKeyInput.h"
#include "app/input/KeyService.h"

// Global instances for configuration and context
GlobalSystemSettings sys_settings;
GlobalPipelineContext sys_context;
HardwareAudioHandles audio_handles;

extern "C" void app_main(void) {
  // 1. Initialize Log Routing
  LogRouter::getInstance().init();
  LOGI_SYSTEM("Initializing System Application Layer...");

  // 2. Initialize Board Hardware (NVS, I2C, IO Expander, I2S, Codecs)
  Board &board = Board::getInstance();
  board.setSampleRate(sys_settings.sample_rate);
  if (!board.begin()) {
    LOGE_SYSTEM("Fatal: Failed to initialize board hardware!");
  } else {
    LOGI_SYSTEM("Board hardware and NVS ready.");
  }

  // 2b. Initialize Key Polling Service
  static ExpanderKeyInput key_input(board.getIoExpanderInstance());
  static KeyService key_service(key_input);
  key_service.start();

  // 3. Initialize Event Bus
  EventBus::getInstance().init();

  // 4. Initialize Application Orchestrator
  // This class handles everything that happens when network
  // connects/disconnects
  AppController::getInstance().begin(sys_settings, sys_context, audio_handles);

  // 5. Configure and Start WiFi
  sys_settings.wifi_ssid = "Airtel_Hacked";
  sys_settings.wifi_password = "hackedhai@407";
  sys_settings.server_ip = "192.168.1.18";

  WifiManager::Config wifi_cfg = {.ssid = sys_settings.wifi_ssid,
                                  .password = sys_settings.wifi_password,
                                  .max_retry = sys_settings.wifi_max_retries};

  static WifiManager wifi(wifi_cfg);
  wifi.begin();

  LOGI_SYSTEM("System initialization complete. Waiting for network events...");
}
