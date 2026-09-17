#pragma once

#include "esp_log.h"
#include <cstdarg>
#include <string>
#include <vector>
#include <utility>
#include "freertos/FreeRTOS.h"

struct WebLogItem {
    uint32_t seq = 0;
    char text[192] = {0};
};

/**
 * @brief Intercepts standard vprintf calls and routes them to console,
 * web ring buffer, and optionally to network.
 */
class LogRouter {
public:
  enum class State { ROUTE_CONSOLE_ONLY, ROUTE_CONSOLE_AND_NETWORK };

  static LogRouter &getInstance();

  /**
   * @brief Initialize the log router and hook into the system vprintf
   */
  void init();

  /**
   * @brief Update the routing state
   */
  void setNetworkStreamingState(State newState);

  /**
   * @brief Retrieve logs newer than since_seq for web console display
   */
  void getLogsSince(uint32_t since_seq, std::vector<std::pair<uint32_t, std::string>>& out_logs, uint32_t& out_latest_seq);

private:
  LogRouter();
  State m_state;
  vprintf_like_t m_default_vprintf;

  static constexpr size_t MAX_LOG_ENTRIES = 80;
  WebLogItem m_ring_buffer[MAX_LOG_ENTRIES];
  size_t m_ring_head = 0;
  size_t m_ring_count = 0;
  uint32_t m_current_seq = 0;
  portMUX_TYPE m_spinlock = portMUX_INITIALIZER_UNLOCKED;

  void pushLog(const char* msg);

  /**
   * @brief Static interceptor hook for esp_log_set_vprintf
   */
  static int vprintfInterceptor(const char *format, va_list args);
};
