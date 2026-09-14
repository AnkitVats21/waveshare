#pragma once
#include <cstdio>

#ifndef ESP_LOG_COLOR_ENABLE
#define ESP_LOG_COLOR_ENABLE 1
#endif

#define ESP_LOGE(tag, fmt, ...) std::fprintf(stderr, "[E][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) std::fprintf(stdout, "[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) std::fprintf(stdout, "[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#define ESP_LOGV(tag, fmt, ...) ((void)0)
