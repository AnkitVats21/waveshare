#include "app/event/EventBus.h"
#include "common/AppLogger.h"
#include "common/events/event_bases.h"

// Define all event bases — declarations live in common/events/event_bases.h
ESP_EVENT_DEFINE_BASE(APP_EVENTS);
ESP_EVENT_DEFINE_BASE(WIFI_SYSTEM_EVENTS);
ESP_EVENT_DEFINE_BASE(MQTT_SYSTEM_EVENTS);
ESP_EVENT_DEFINE_BASE(AUDIO_SYSTEM_EVENTS);
ESP_EVENT_DEFINE_BASE(ASSISTANT_EVENTS);


EventBus::EventBus() : custom_loop(nullptr) {}

EventBus &EventBus::getInstance() {
  static EventBus instance;
  return instance;
}

void EventBus::init() {
  LOGI_SYSTEM("Initializing Application Event Bus...");
  esp_event_loop_args_t loop_args = {
      .queue_size      = 10,
      .task_name       = "app_event_loop_task",
      .task_priority   = 5,
      .task_stack_size = 4096,
      .task_core_id    = 0,
  };
  esp_event_loop_create(&loop_args, &custom_loop);
}
