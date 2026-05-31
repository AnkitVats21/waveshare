#include "SystemContext.h"
#include "sdkconfig.h"
#include "services/BufferManager.h"

SystemContext &SystemContext::get() {
  static SystemContext instance;
  return instance;
}

void SystemContext::init() {
  // ---- Wi-Fi -----------------------------------------------------------
  settings.wifi_ssid       = CONFIG_WAVESHARE_WIFI_SSID;
  settings.wifi_password   = CONFIG_WAVESHARE_WIFI_PASSWORD;
  settings.wifi_max_retries = CONFIG_WAVESHARE_WIFI_MAXIMUM_RETRY;

  // ---- Network / streaming ---------------------------------------------
  settings.server_ip   = CONFIG_WAVESHARE_SERVER_IP;
  settings.tx_rtp_port = CONFIG_WAVESHARE_RTP_TX_PORT;
  settings.rx_rtp_port = CONFIG_WAVESHARE_RTP_RX_PORT;

  // ---- Hardware defaults (fixed for this board) -------------------------
  // sample_rate, mic_enabled, task priorities all stay at their
  // GlobalSystemSettings defaults unless overridden via NVS in future.

  // ---- Hardware handles start null; Board::begin() fills them ----------
  hw = {};

  // ---- Allocate all PSRAM ring buffers registered via DECLARE/DEFINE_BUFFER -
  // This must run after static-init (which registered the descriptors) and
  // before any audio pipeline task starts.
  if (!BufferManager::getInstance().initAll()) {
    // Non-fatal at this point — pipeline manager will catch nullptrs
    // and log errors when it tries to use the handles.
  }
}
