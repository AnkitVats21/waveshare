#include "AppController.h"
#include "SystemContext.h"
#include "app/audio/AudioPipelineManager.h"
#include "app/audio/AudioService.h"
#include "app/event/EventBus.h"
#include "app/led/LedService.h"
#include "app/mqtt/MqttTask.h"
#include "app/wake_word/WakeWordDetector.h"
#include "common/AppLogger.h"
#include "common/AsyncNetLogger.h"
#include "common/LogRouter.h"
#include "common/events/app_events.h"
#include "common/events/wifi_events.h"
#include "hal/Board.h"
#include "sdkconfig.h"

AppController &AppController::getInstance() {
  static AppController instance;
  return instance;
}

bool AppController::onStart() {
  // Start LED service first — it runs independently of the network
  LedService::getInstance().begin(&Board::getInstance(),
                                  &EventBus::getInstance());

  // Subscribe to Wi-Fi lifecycle events via IService helper
  subscribeEvent(WIFI_SYSTEM_EVENTS, WifiEvent::CONNECTED);
  subscribeEvent(WIFI_SYSTEM_EVENTS, WifiEvent::DISCONNECTED);

  // Signal "not yet connected" with a red LED
  LedEventData init_led = {LedMode::SOLID, RED_LED, 0, 0};
  EventBus::getInstance().publish(APP_EVENTS, AppEvent::LED_COMMAND, init_led);

#ifdef CONFIG_WAVESHARE_WAKEWORD_ENABLE
  // Wake-word detector starts before WiFi — needs Board audio + AudioService listener
  // AudioService implements IWakeWordListener; Board implements IAudioFeedSource.
  auto &wwd = WakeWordDetector::getInstance();
  wwd.setFeedSource(&Board::getInstance());
  wwd.setListener(&AudioService::getInstance());
  
  if (wwd.begin()) {
    LOGI_SYSTEM("WakeWordDetector started.");
  } else {
    LOGW_SYSTEM("WakeWordDetector failed to start (check 'model' partition).");
  }
#endif

  LOGI_SYSTEM("AppController initialized and listening for system events.");
  return true;
}

void AppController::onStop() {
  teardownNetworkServices();
}

void AppController::onEvent(esp_event_base_t base, int32_t id, void * /*data*/) {
  if (base == WIFI_SYSTEM_EVENTS) {
    if (id == static_cast<int32_t>(WifiEvent::CONNECTED)) {
      LOGI_SYSTEM("Network connection established. Starting bootstrap...");

      LedEventData green_led = {LedMode::SOLID, GREEN_LED, 0, 0};
      EventBus::getInstance().publish(APP_EVENTS, AppEvent::LED_COMMAND,
                                      green_led);

      // Setup network logging
      auto &ctx = SystemContext::get();
      AsyncNetLogger::getInstance().init(ctx.settings.server_ip, 5006);
      AsyncNetLogger::getInstance().startWorker();
      LogRouter::getInstance().setNetworkStreamingState(
          LogRouter::State::ROUTE_CONSOLE_AND_NETWORK);

      bootstrapAudio();

      LedEventData blue_blink = {LedMode::BLINK, BLUE_LED, 250, 2};
      EventBus::getInstance().publish(APP_EVENTS, AppEvent::LED_COMMAND,
                                      blue_blink);
      vTaskDelay(pdMS_TO_TICKS(1100));

      initMqtt();

      LedEventData purple_blink = {LedMode::BLINK, PURPLE_LED, 250, 2};
      EventBus::getInstance().publish(APP_EVENTS, AppEvent::LED_COMMAND,
                                      purple_blink);
      vTaskDelay(pdMS_TO_TICKS(1100));

    } else if (id == static_cast<int32_t>(WifiEvent::DISCONNECTED)) {
      LOGW_WIFI("Network connection lost. Tearing down network services.");
      teardownNetworkServices();

      LedEventData red_led = {LedMode::BLINK, RED_LED, 500, 0};
      EventBus::getInstance().publish(APP_EVENTS, AppEvent::LED_COMMAND,
                                      red_led);
    }
  }
}

void AppController::bootstrapAudio() {
  auto &ctx = SystemContext::get();
  bool ok = AudioService::getInstance().begin(ctx.settings, ctx.hw,
                                              &Board::getInstance());
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
  auto &ctx = SystemContext::get();
  if (MqttTask::getInstance().init(ctx.settings)) {
    LOGI_SYSTEM("MQTT Task started successfully.");
  } else {
    LOGE_SYSTEM("Failed to start MQTT Task.");
  }
}
