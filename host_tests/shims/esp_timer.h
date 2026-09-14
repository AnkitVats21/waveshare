#pragma once
#include <cstdint>
#include <chrono>

inline int64_t esp_timer_get_time() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}
