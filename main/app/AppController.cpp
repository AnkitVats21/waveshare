#include "AppController.h"
#include "app/audio/AudioPipelineManager.h"
#include "app/audio/AudioService.h"
#include "app/mqtt/MqttTask.h"
#include "app/wake_word/WakeWordDetector.h"
#include "common/AppLogger.h"
#include "common/AsyncNetLogger.h"
#include "common/LogRouter.h"
#include "hal/Board.h"
#include "hal/network/WifiManager.h"
#include "sdkconfig.h"
#include "app/event/EventBus.h"
#include "app/led/LedService.h"

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

  // Initialize LED task service (runs asynchronously at low priority)
  LedService::getInstance().begin(&Board::getInstance(), &EventBus::getInstance());

  // Subscribe to WiFi events via the EventBus
  EventBus::getInstance().subscribe(WIFI_SYSTEM_EVENTS, WifiEvent::CONNECTED,
                                    &AppController::onNetworkReady, this);
  EventBus::getInstance().subscribe(WIFI_SYSTEM_EVENTS, WifiEvent::DISCONNECTED,
                                    &AppController::onNetworkLost, this);

  // Initializing the board setting the LED color to red (GRB: G=0, R=80 => red)
  LedEventData init_led = {LedMode::SOLID, RED_LED, 0, 0};
  EventBus::getInstance().publish(APP_EVENTS, AppEvent::LED_COMMAND, init_led);

#ifdef CONFIG_WAVESHARE_WAKEWORD_ENABLE
  // Start wake-word detector immediately — it only needs Board (audio HW),
  // not WiFi.  The detector fires EventBus WAKE_WORD_DETECTED events.
  if (WakeWordDetector::getInstance().begin()) {
    LOGI_SYSTEM("WakeWordDetector started.");
  } else {
    LOGW_SYSTEM("WakeWordDetector failed to start (check 'model' partition).");
  }
#endif

  LOGI_SYSTEM("AppController initialized and listening for system events.");
}

void AppController::onNetworkReady(void *handler_arg, esp_event_base_t base,
                                   int32_t id, void *event_data) {
  AppController *self = static_cast<AppController *>(handler_arg);
  LOGI_SYSTEM("Network connection established. Starting bootstrap...");
  
  // Network connected: set LED color to green (GRB: R=80 => green)
  LedEventData green_led = {LedMode::SOLID, GREEN_LED, 0, 0};
  EventBus::getInstance().publish(APP_EVENTS, AppEvent::LED_COMMAND, green_led);

  // 1. Setup Network Logging
  // TODO: Move the hardcoded logger udp port to the config 
// #ifdef NETWORK_LOGGER
  AsyncNetLogger::getInstance().init(self->m_settings->server_ip, 5006);
  AsyncNetLogger::getInstance().startWorker();
  LogRouter::getInstance().setNetworkStreamingState(
      LogRouter::State::ROUTE_CONSOLE_AND_NETWORK);
// #endif
  // 2. Bootstrap Audio System
  self->bootstrapAudio();
  
  // Audio system started: blink the LED blue 2 times
  LedEventData blue_blink = {LedMode::BLINK, BLUE_LED, 250, 2};
  EventBus::getInstance().publish(APP_EVENTS, AppEvent::LED_COMMAND, blue_blink);
  vTaskDelay(pdMS_TO_TICKS(1100)); // Let the blink pattern complete before next animation

  // 3. Initialize MQTT
  self->initMqtt();
  
  // MQTT Task started: blink the LED purple/magenta 2 times
  LedEventData purple_blink = {LedMode::BLINK, PURPLE_LED, 250, 2};
  EventBus::getInstance().publish(APP_EVENTS, AppEvent::LED_COMMAND, purple_blink);
  vTaskDelay(pdMS_TO_TICKS(1100));
}

void AppController::onNetworkLost(void *handler_arg, esp_event_base_t base,
                                  int32_t id, void *event_data) {
  AppController *self = static_cast<AppController *>(handler_arg);
  LOGW_WIFI("Network connection lost. Tearing down network services.");

  self->teardownNetworkServices();
  
  // Set LED color to RED (GRB: G=80 => red)
  LedEventData red_led = {LedMode::BLINK, RED_LED, 0, 0};
  EventBus::getInstance().publish(APP_EVENTS, AppEvent::LED_COMMAND, red_led);
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
