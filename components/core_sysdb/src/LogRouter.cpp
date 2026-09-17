#include "core_sysdb/LogRouter.h"
#include "core_sysdb/AsyncNetLogger.h"
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

void LogRouter::pushLog(const char *msg) {
  if (!msg || msg[0] == '\0') return;
  portENTER_CRITICAL(&m_spinlock);
  m_current_seq++;
  WebLogItem &item = m_ring_buffer[m_ring_head];
  item.seq = m_current_seq;
  strncpy(item.text, msg, sizeof(item.text) - 1);
  item.text[sizeof(item.text) - 1] = '\0';

  // Strip trailing whitespace and newlines
  size_t len = strlen(item.text);
  while (len > 0 && (item.text[len - 1] == '\r' || item.text[len - 1] == '\n')) {
    item.text[len - 1] = '\0';
    len--;
  }

  m_ring_head = (m_ring_head + 1) % MAX_LOG_ENTRIES;
  if (m_ring_count < MAX_LOG_ENTRIES) {
    m_ring_count++;
  }
  portEXIT_CRITICAL(&m_spinlock);
}

void LogRouter::getLogsSince(uint32_t since_seq,
                             std::vector<std::pair<uint32_t, std::string>> &out_logs,
                             uint32_t &out_latest_seq) {
  out_logs.clear();
  portENTER_CRITICAL(&m_spinlock);
  out_latest_seq = m_current_seq;
  if (m_ring_count == 0) {
    portEXIT_CRITICAL(&m_spinlock);
    return;
  }
  size_t start_idx = (m_ring_head + MAX_LOG_ENTRIES - m_ring_count) % MAX_LOG_ENTRIES;
  for (size_t i = 0; i < m_ring_count; ++i) {
    size_t idx = (start_idx + i) % MAX_LOG_ENTRIES;
    if (m_ring_buffer[idx].seq > since_seq) {
      out_logs.emplace_back(m_ring_buffer[idx].seq, std::string(m_ring_buffer[idx].text));
    }
  }
  portEXIT_CRITICAL(&m_spinlock);
}

int LogRouter::vprintfInterceptor(const char *format, va_list args) {
  LogRouter &self = LogRouter::getInstance();

  char log_buffer[256];
  int written = vsnprintf(log_buffer, sizeof(log_buffer), format, args);

  if (self.m_default_vprintf && written > 0) {
    printf("%s", log_buffer);
  }

  if (written > 0) {
    self.pushLog(log_buffer);
  }

  // Duplicate to UDP queue ONLY if state flag registers active
  if (self.m_state == State::ROUTE_CONSOLE_AND_NETWORK) {
    AsyncNetLogger::getInstance().queueLogMessage(log_buffer);
  }
  return written;
}
