#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cassert>

typedef uint32_t TickType_t;
typedef int32_t  BaseType_t;
typedef uint32_t UBaseType_t;

#define pdTRUE  (1)
#define pdFALSE (0)
#define pdPASS  (1)
#define pdFAIL  (0)

#define portMAX_DELAY (0xFFFFFFFFUL)
#define portTICK_PERIOD_MS (1)
#define pdMS_TO_TICKS(ms) (ms)

#define configASSERT(x) assert(x)

#define IRAM_ATTR
#define DRAM_ATTR
