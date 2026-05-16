#include "LogRouter.h"
#include "AsyncNetLogger.h"
#include <cstdio>
#include <cstring>

LogRouter &LogRouter::getInstance() {
  static LogRouter instance;
  return instance;
}

LogRouter::LogRouter()
    : m_state(State::ROUTE_CONSOLE_ONLY), m_default_vprintf(nullptr) {}

void LogRouter::init() {
  m_state = State::ROUTE_CONSOLE_ONLY;
  m_default_vprintf = esp_log_set_vprintf(&LogRouter::vprintfInterceptor);
}

void LogRouter::setNetworkStreamingState(State newState) { m_state = newState; }

int LogRouter::vprintfInterceptor(const char *format, va_list args) {
  LogRouter &self = LogRouter::getInstance();

  // 1. Core Action: Always render logs onto physical UART serial console line
  // first
  char log_buffer[256];
  int written = vsnprintf(log_buffer, sizeof(log_buffer), format, args);

  if (self.m_default_vprintf && written > 0) {
    // Note: Use printf here instead of m_default_vprintf to avoid recursion
    printf("%s", log_buffer);
  }

  // 2. Duplicate to UDP queue ONLY if state flag registers active
  if (self.m_state == State::ROUTE_CONSOLE_AND_NETWORK) {
    AsyncNetLogger::getInstance().queueLogMessage(log_buffer);
  }
  return written;
}
