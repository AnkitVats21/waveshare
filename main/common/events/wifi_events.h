#pragma once

#include "common/events/event_bases.h"
#include <cstdint>

/**
 * @brief Wi-Fi connectivity event IDs.
 *
 * Published by WifiManager when the network interface changes state.
 * Subscribe via: IService::subscribeEvent(WIFI_SYSTEM_EVENTS, WifiEvent::XXX)
 */
enum class WifiEvent : int32_t {
  CONNECTED,    ///< IP address obtained; network is usable
  DISCONNECTED, ///< Link lost or max retries exceeded
};
