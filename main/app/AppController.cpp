#include "AppController.h"
#include "SystemContext.h"
#include "app/audio/AudioPipelineManager.h"
#include "app/audio/AudioService.h"
#include "app/event/EventBus.h"
#include "app/gemini_live/GeminiLiveService.h"
#include "app/gemini_live/gemini_skills_generated.h"
#include "app/led/LedService.h"
#include "app/mqtt/MqttTask.h"
#include "app/wake_word/WakeWordDetector.h"
#include "common/AppLogger.h"
#include "common/AsyncNetLogger.h"
#include "common/LogRouter.h"
#include "common/events/app_events.h"
#include "common/events/wifi_events.h"
#include "hal/Board.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include <string>

#include "esp_sntp.h"
#include "esp_netif_sntp.h"
#include <sys/time.h>

#if defined(CONFIG_VOICE_BACKEND_GEMINI_LIVE)
#include "app/gemini_live/GeminiLiveService.h"
#endif

// Manually sets baseline date to 2026 to ensure TLS cert verification success, 
// then boots SNTP client in background to fetch true NTP time.
// static void initialize_sntp_and_set_time() {
//     // 1. Manually set baseline epoch time to early 2026 (1775000000 epoch seconds)
//     struct timeval tv;
//     tv.tv_sec = 1775000000;
//     tv.tv_usec = 0;
//     settimeofday(&tv, nullptr);
//     LOGI_SYSTEM("System clock manually initialized to early 2026 baseline epoch.");

//     // 2. Register background SNTP client
//     esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
//     esp_netif_sntp_init(&sntp_cfg);
// }

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
  
  // Subscribe to Gemini Tool Calls
  subscribeEvent(APP_EVENTS, AppEvent::GEMINI_TOOL_CALL);

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
  time_t now;
  time(&now);
  LOGI_SYSTEM("Current epoch=%lld", (long long)now);
  return true;
}

void AppController::onStop() {
  teardownNetworkServices();
}

void AppController::onEvent(esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_SYSTEM_EVENTS) {
    if (id == static_cast<int32_t>(WifiEvent::CONNECTED)) {
      LOGI_SYSTEM("Network connection established. Starting bootstrap...");

      // Automatically set 2026 baseline time and spawn SNTP background sync
      // (This is a stub for Production TLS verification. Uncomment this call when enabling USE_PRODUCTION_SECURE_TLS)
      // initialize_sntp_and_set_time();

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
  } else if (base == APP_EVENTS) {
      if (id == static_cast<int32_t>(AppEvent::GEMINI_TOOL_CALL)) {
          // Because publish took 'skill_data' (which is DecodedSkillCall* pointer), the event loop stored the pointer.
          // EventBus provides the address of the stored data, so 'data' is DecodedSkillCall**
          auto* skill_ptr_ptr = static_cast<GeminiSkills::DecodedSkillCall**>(data);
          if (!skill_ptr_ptr || !*skill_ptr_ptr) return;
          auto* skill_call = *skill_ptr_ptr;
          
          cJSON* response_root = cJSON_CreateObject();
          
          // Use type-safe generated Enums for dispatch switching
          switch (skill_call->type) {
              
              case GeminiSkills::SkillType::ADJUST_HARDWARE_VOLUME: {
                  // Direct type-safe member parameter access
                  int volume = skill_call->args.adjust_hardware_volume->volume_level;
                  
                  Board::getInstance().setPlayVolume(volume);
                  cJSON_AddStringToObject(response_root, "status", "success");
                  cJSON_AddNumberToObject(response_root, "new_volume", volume);
                  break;
              }
              
              case GeminiSkills::SkillType::GET_CURRENT_WEATHER: {
                  std::string city = skill_call->args.get_current_weather->location;
                  LOGI_SYSTEM("Checking weather condition telemetry for city: %s", city.c_str());
                  
                  cJSON_AddStringToObject(response_root, "status", "success");
                  cJSON_AddStringToObject(response_root, "condition", "Sunny");
                  cJSON_AddNumberToObject(response_root, "temperature", 72);
                  break;
              }
              
              default:
                  cJSON_AddStringToObject(response_root, "status", "error");
                  cJSON_AddStringToObject(response_root, "message", "Unsupported or unknown skill channel");
                  break;
          }
          
          char* feedback_string = cJSON_PrintUnformatted(response_root);
          if (feedback_string) {
#if defined(CONFIG_VOICE_BACKEND_GEMINI_LIVE)
              // Transmit acknowledgment structure natively back down into the open stream
              GeminiLiveService::getInstance().sendToolExecutionReceipt(skill_call->call_id, feedback_string);
#endif
              cJSON_free(feedback_string);
          }
          cJSON_Delete(response_root);
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

#if defined(CONFIG_VOICE_BACKEND_GEMINI_LIVE)
    LOGI_SYSTEM("Bootstrapping Gemini Live transport...");
    GeminiLiveService::getInstance().onStart();
#elif defined(CONFIG_VOICE_BACKEND_RTP)
    LOGI_SYSTEM("RTP Backend is active. Transports managed by AudioPipelineManager.");
#endif
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
