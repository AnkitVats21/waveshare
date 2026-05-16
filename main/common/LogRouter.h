#pragma once

#include "esp_log.h"
#include <cstdarg>

/**
 * @brief Intercepts standard vprintf calls and routes them to console and
 * optionally to network
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

private:
  LogRouter();
  State m_state;
  vprintf_like_t m_default_vprintf;

  /**
   * @brief Static interceptor hook for esp_log_set_vprintf
   */
  static int vprintfInterceptor(const char *format, va_list args);
};
