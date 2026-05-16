#include "AppController.h"
#include "app/audio/AudioPipelineManager.h"
#include "app/audio/AudioService.h"
#include "common/AppLogger.h"
#include "common/AsyncNetLogger.h"
#include "common/LogRouter.h"
#include "hal/Board.h"
#include "hal/network/WifiManager.h"
#include "services/EventBus.h"
#include "app/mqtt/MqttTask.h"

AppController &AppController::getInstance() {
  static AppController instance;
  return instance;
}

void AppController::begin(GlobalSystemSettings &settings,
                          GlobalPipelineContext &context,
                          HardwareAudioHandles &handles) {
  m_settings = &settings;
  m_context = &context;
  m_handles = &handles;

  // Subscribe to WiFi events via the EventBus
  EventBus::getInstance().subscribe(WIFI_SYSTEM_EVENTS, WifiEvent::CONNECTED,
                                    &AppController::onNetworkReady, this);
  EventBus::getInstance().subscribe(WIFI_SYSTEM_EVENTS, WifiEvent::DISCONNECTED,
                                    &AppController::onNetworkLost, this);

  // Initialize LED color directly via Board
  Board::getInstance().setAllLedsColor(0, 80, 0);

  LOGI_SYSTEM("AppController initialized and listening for system events.");
}

void AppController::onNetworkReady(void *handler_arg, esp_event_base_t base,
                                   int32_t id, void *event_data) {
  AppController *self = static_cast<AppController *>(handler_arg);
  LOGI_SYSTEM("Network connection established. Starting bootstrap...");

  // 1. Setup Network Logging
  AsyncNetLogger::getInstance().init(self->m_settings->server_ip, 5005);
  AsyncNetLogger::getInstance().startWorker();
  LogRouter::getInstance().setNetworkStreamingState(
      LogRouter::State::ROUTE_CONSOLE_AND_NETWORK);

  // 2. Bootstrap Audio System
  self->bootstrapAudio();

  // 3. Initialize MQTT
  self->initMqtt();
}

void AppController::onNetworkLost(void *handler_arg, esp_event_base_t base,
                                  int32_t id, void *event_data) {
  AppController *self = static_cast<AppController *>(handler_arg);
  LOGW_WIFI("Network connection lost. Tearing down network services.");

  self->teardownNetworkServices();
}

void AppController::bootstrapAudio() {
  // Initialize Unified Audio Service (Hardware + Pipeline + Logic)
  bool ok = AudioService::getInstance().begin(*m_settings, *m_context,
                                              *m_handles, &Board::getInstance(),
                                              &EventBus::getInstance());

  if (ok) {
    LOGI_AUDIO("Unified Audio Service initialized successfully.");
  } else {
    LOGE_AUDIO("Failed to initialize Audio Service.");
  }
}

void AppController::teardownNetworkServices() {
  LogRouter::getInstance().setNetworkStreamingState(
      LogRouter::State::ROUTE_CONSOLE_ONLY);
  AsyncNetLogger::getInstance().stopWorker();
}

void AppController::initMqtt() {
  LOGI_SYSTEM("Initializing MQTT Task...");
  if (MqttTask::getInstance().init(*m_settings)) {
    LOGI_SYSTEM("MQTT Task started successfully.");
  } else {
    LOGE_SYSTEM("Failed to start MQTT Task.");
  }
}
