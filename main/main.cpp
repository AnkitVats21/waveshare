#include "app/AppController.h"
#include "app/SystemContext.h"
#include "app/event/EventBus.h"
#include "app/input/KeyService.h"
#include "common/AppLogger.h"
#include "common/LogRouter.h"
#include "hal/Board.h"
#include "hal/input/ExpanderKeyInput.h"
#include "hal/network/WifiManager.h"

extern "C" void app_main(void) {
  // 1. Initialize Log Routing
  LogRouter::getInstance().init();
  LOGI_SYSTEM("Initializing System Application Layer...");

  // 2. Load system configuration from Kconfig
  SystemContext &ctx = SystemContext::get();
  ctx.init();

  // 3. Initialize Board Hardware (NVS, I2C, IO Expander, I2S, Codecs)
  Board &board = Board::getInstance();
  board.setSampleRate(ctx.settings.sample_rate);
  if (!board.begin()) {
    LOGE_SYSTEM("Fatal: Failed to initialize board hardware!");
  } else {
    LOGI_SYSTEM("Board hardware and NVS ready.");
  }

  // 4. Populate hardware handles into context (for AudioService)
  ctx.hw.mic_rx_handle     = board.getRxHandle();
  ctx.hw.speaker_tx_handle = board.getTxHandle();
  ctx.hw.play_dev          = board.getPlayDev();
  ctx.hw.record_dev        = board.getRecordDev();

  // 5. Initialize Key Polling Service
  static ExpanderKeyInput key_input(board.getIoExpanderInstance());
  static KeyService key_service(key_input);
  key_service.start();

  // 6. Initialize Event Bus
  EventBus::getInstance().init();

  // 7. Initialize Application Orchestrator
  // Handles everything that happens when network connects/disconnects
  AppController::getInstance().begin();

  // 8. Configure and Start WiFi (credentials come from Kconfig via SystemContext)
  WifiManager::Config wifi_cfg = {
      .ssid     = ctx.settings.wifi_ssid,
      .password = ctx.settings.wifi_password,
      .max_retry = ctx.settings.wifi_max_retries,
  };

  static WifiManager wifi(wifi_cfg);
  wifi.begin();

  LOGI_SYSTEM("System initialization complete. Waiting for network events...");
}
