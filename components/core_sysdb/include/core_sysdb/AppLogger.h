#pragma once
#include "esp_log.h"

// Unified system module tags
#define LOG_TAG_WIFI "WIFI"
#define LOG_TAG_AUDIO "AUDIO"
#define LOG_TAG_NET "NET"
#define LOG_TAG_SYSTEM "SYSTEM"
#define LOG_TAG_HAL "HAL"

// Info (LOGI)
#define LOGI_WIFI(format, ...) ESP_LOGI(LOG_TAG_WIFI, format, ##__VA_ARGS__)
#define LOGI_AUDIO(format, ...) ESP_LOGI(LOG_TAG_AUDIO, format, ##__VA_ARGS__)
#define LOGI_NET(format, ...) ESP_LOGI(LOG_TAG_NET, format, ##__VA_ARGS__)
#define LOGI_SYSTEM(format, ...) ESP_LOGI(LOG_TAG_SYSTEM, format, ##__VA_ARGS__)
#define LOGI_HAL(format, ...) ESP_LOGI(LOG_TAG_HAL, format, ##__VA_ARGS__)

// Error (LOGE)
#define LOGE_WIFI(format, ...) ESP_LOGE(LOG_TAG_WIFI, format, ##__VA_ARGS__)
#define LOGE_AUDIO(format, ...) ESP_LOGE(LOG_TAG_AUDIO, format, ##__VA_ARGS__)
#define LOGE_NET(format, ...) ESP_LOGE(LOG_TAG_NET, format, ##__VA_ARGS__)
#define LOGE_SYSTEM(format, ...) ESP_LOGE(LOG_TAG_SYSTEM, format, ##__VA_ARGS__)
#define LOGE_HAL(format, ...) ESP_LOGE(LOG_TAG_HAL, format, ##__VA_ARGS__)

// Warning (LOGW)
#define LOGW_WIFI(format, ...) ESP_LOGW(LOG_TAG_WIFI, format, ##__VA_ARGS__)
#define LOGW_AUDIO(format, ...) ESP_LOGW(LOG_TAG_AUDIO, format, ##__VA_ARGS__)
#define LOGW_NET(format, ...) ESP_LOGW(LOG_TAG_NET, format, ##__VA_ARGS__)
#define LOGW_SYSTEM(format, ...) ESP_LOGW(LOG_TAG_SYSTEM, format, ##__VA_ARGS__)
#define LOGW_HAL(format, ...) ESP_LOGW(LOG_TAG_HAL, format, ##__VA_ARGS__)

// Debug (LOGD)
#define LOGD_WIFI(format, ...) ESP_LOGD(LOG_TAG_WIFI, format, ##__VA_ARGS__)
#define LOGD_AUDIO(format, ...) ESP_LOGD(LOG_TAG_AUDIO, format, ##__VA_ARGS__)
#define LOGD_NET(format, ...) ESP_LOGD(LOG_TAG_NET, format, ##__VA_ARGS__)
#define LOGD_SYSTEM(format, ...) ESP_LOGD(LOG_TAG_SYSTEM, format, ##__VA_ARGS__)
#define LOGD_HAL(format, ...) ESP_LOGD(LOG_TAG_HAL, format, ##__VA_ARGS__)
