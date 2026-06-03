#pragma once

#include "esp_event.h"

/**
 * @brief Single source of truth for all ESP-IDF custom event base declarations.
 *
 * Every event base used in the system is declared here.
 * The corresponding ESP_EVENT_DEFINE_BASE() definitions live in EventBus.cpp.
 *
 * Include this header (directly or via an event-specific header) wherever
 * you need to publish or subscribe to system events.
 */
ESP_EVENT_DECLARE_BASE(APP_EVENTS);
ESP_EVENT_DECLARE_BASE(WIFI_SYSTEM_EVENTS);
ESP_EVENT_DECLARE_BASE(MQTT_SYSTEM_EVENTS);
ESP_EVENT_DECLARE_BASE(AUDIO_SYSTEM_EVENTS);
ESP_EVENT_DECLARE_BASE(ASSISTANT_EVENTS);
