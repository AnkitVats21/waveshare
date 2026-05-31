#pragma once

/**
 * @file app_types.h
 * @brief DEPRECATED umbrella header — do not add new content here.
 *
 * This file exists only for backward compatibility during the refactor.
 * All types have been moved to focused headers:
 *
 *   Hardware handles  → common/hw_types.h
 *   System settings   → common/system_settings.h
 *   LED types/colors  → common/led_types.h
 *   Event bases       → common/events/event_bases.h
 *   App events        → common/events/app_events.h
 *   Wi-Fi events      → common/events/wifi_events.h
 *   MQTT events       → common/events/mqtt_events.h
 *
 * GlobalPipelineContext has been removed — ring buffers are now owned by
 * BufferManager. Use BufferManager::getInstance().handle(Buffers::MIC_TX_BUF)
 * and BufferManager::getInstance().handle(Buffers::SPK_RX_BUF) instead.
 *
 * Migrate each consumer to include only what it needs, then delete this file.
 */

#include "common/hw_types.h"
#include "common/system_settings.h"
#include "common/led_types.h"
#include "common/events/event_bases.h"
#include "common/events/app_events.h"
#include "common/events/wifi_events.h"
#include "common/events/mqtt_events.h"

