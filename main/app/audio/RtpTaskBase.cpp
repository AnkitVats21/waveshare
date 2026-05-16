#include "RtpTaskBase.h"
#include "common/AppLogger.h"
#include "lwip/sockets.h"

RtpTaskBase::RtpTaskBase(const CommonConfig &config,
                         RingbufHandle_t ring_buffer, const char *log_tag)
    : m_config(config), m_ring_buffer(ring_buffer), m_task_handle(nullptr),
      m_socket(-1), m_tag(log_tag) {}

RtpTaskBase::~RtpTaskBase() { stop(); }

bool RtpTaskBase::begin() {
  BaseType_t result = xTaskCreatePinnedToCore(
      &RtpTaskBase::taskWrapper, m_tag, m_config.stack_size, this,
      m_config.priority, &m_task_handle, m_config.core_id);
  return (result == pdPASS);
}

void RtpTaskBase::stop() {
  if (m_task_handle != nullptr) {
    vTaskDelete(m_task_handle);
    m_task_handle = nullptr;
  }
  closeSocket();
}

void RtpTaskBase::closeSocket() {
  if (m_socket >= 0) {
    close(m_socket);
    m_socket = -1;
  }
}

void RtpTaskBase::taskWrapper(void *pvParameters) {
  RtpTaskBase *instance = static_cast<RtpTaskBase *>(pvParameters);
  instance->processLoop();
}
