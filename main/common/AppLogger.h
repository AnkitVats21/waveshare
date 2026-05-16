#pragma once
#include "esp_log.h"

// Unified system module tags
#define TAG_WIFI "WIFI"
#define TAG_AUDIO "AUDIO"
#define TAG_NET "NET"
#define TAG_SYSTEM "SYSTEM"
#define TAG_HAL "HAL"

// Info (LOGI)
#define LOGI_WIFI(format, ...) ESP_LOGI(TAG_WIFI, format, ##__VA_ARGS__)
#define LOGI_AUDIO(format, ...) ESP_LOGI(TAG_AUDIO, format, ##__VA_ARGS__)
#define LOGI_NET(format, ...) ESP_LOGI(TAG_NET, format, ##__VA_ARGS__)
#define LOGI_SYSTEM(format, ...) ESP_LOGI(TAG_SYSTEM, format, ##__VA_ARGS__)
#define LOGI_HAL(format, ...) ESP_LOGI(TAG_HAL, format, ##__VA_ARGS__)

// Error (LOGE)
#define LOGE_WIFI(format, ...) ESP_LOGE(TAG_WIFI, format, ##__VA_ARGS__)
#define LOGE_AUDIO(format, ...) ESP_LOGE(TAG_AUDIO, format, ##__VA_ARGS__)
#define LOGE_NET(format, ...) ESP_LOGE(TAG_NET, format, ##__VA_ARGS__)
#define LOGE_SYSTEM(format, ...) ESP_LOGE(TAG_SYSTEM, format, ##__VA_ARGS__)
#define LOGE_HAL(format, ...) ESP_LOGE(TAG_HAL, format, ##__VA_ARGS__)

// Warning (LOGW)
#define LOGW_WIFI(format, ...) ESP_LOGW(TAG_WIFI, format, ##__VA_ARGS__)
#define LOGW_AUDIO(format, ...) ESP_LOGW(TAG_AUDIO, format, ##__VA_ARGS__)
#define LOGW_NET(format, ...) ESP_LOGW(TAG_NET, format, ##__VA_ARGS__)
#define LOGW_SYSTEM(format, ...) ESP_LOGW(TAG_SYSTEM, format, ##__VA_ARGS__)
#define LOGW_HAL(format, ...) ESP_LOGW(TAG_HAL, format, ##__VA_ARGS__)

// Debug (LOGD)
#define LOGD_WIFI(format, ...) ESP_LOGD(TAG_WIFI, format, ##__VA_ARGS__)
#define LOGD_AUDIO(format, ...) ESP_LOGD(TAG_AUDIO, format, ##__VA_ARGS__)
#define LOGD_NET(format, ...) ESP_LOGD(TAG_NET, format, ##__VA_ARGS__)
#define LOGD_SYSTEM(format, ...) ESP_LOGD(TAG_SYSTEM, format, ##__VA_ARGS__)
#define LOGD_HAL(format, ...) ESP_LOGD(TAG_HAL, format, ##__VA_ARGS__)
