#pragma once

#include "common/events/event_bases.h"
#include <cstdint>

/**
 * @brief MQTT connectivity and data event IDs.
 *
 * Published by MqttService.
 * Subscribe via: IService::subscribeEvent(MQTT_SYSTEM_EVENTS, MqttEvent::XXX)
 */
enum class MqttEvent : int32_t {
  CONNECTED,             ///< Broker connection established
  DISCONNECTED,          ///< Broker connection lost
  OUTGOING_DATA_SUBMIT,  ///< Caller wants to publish data to the broker
  INCOMING_DATA_RECEIVED,///< Data arrived from a subscribed topic
};
