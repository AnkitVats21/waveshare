#include "RtpTaskBase.h"
#include "common/AppLogger.h"
#include "lwip/sockets.h"

RtpTaskBase::RtpTaskBase(const CommonConfig &config,
                         RingbufHandle_t ring_buffer, const char *log_tag)
    : m_config(config), m_ring_buffer(ring_buffer), m_task_handle(nullptr),
      m_socket(-1), m_tag(log_tag), m_is_running(false) {}

RtpTaskBase::~RtpTaskBase() { stop(); }

bool RtpTaskBase::begin() {
  if (m_is_running) return true;
  m_is_running = true;
  BaseType_t result = xTaskCreatePinnedToCore(
      &RtpTaskBase::taskWrapper, m_tag, m_config.stack_size, this,
      m_config.priority, &m_task_handle, m_config.core_id);
  return (result == pdPASS);
}

void RtpTaskBase::stop() {
  m_is_running = false;
  
  // Close socket to unblock any pending recvfrom/sendto
  closeSocket();

  // We give some time for the task to exit itself, 
  // but we don't vTaskDelete from here to avoid resource leaks
  // or crashing if the task is in a library call.
  // The task itself will call vTaskDelete(NULL) when it sees m_is_running == false.
  
  // Wait a bit for the task to cleanup
  vTaskDelay(pdMS_TO_TICKS(50));
  m_task_handle = nullptr;
}

void RtpTaskBase::closeSocket() {
  if (m_socket >= 0) {
    // shutdown(m_socket, SHUT_RDWR); // Force unblock
    close(m_socket);
    m_socket = -1;
  }
}

void RtpTaskBase::taskWrapper(void *pvParameters) {
  RtpTaskBase *instance = static_cast<RtpTaskBase *>(pvParameters);
  instance->processLoop();
  instance->m_task_handle = nullptr;
  vTaskDelete(NULL);
}
