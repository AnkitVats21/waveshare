#include "AsyncNetLogger.h"
#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include <cstring>

AsyncNetLogger &AsyncNetLogger::getInstance() {
  static AsyncNetLogger instance;
  return instance;
}

AsyncNetLogger::AsyncNetLogger()
    : m_log_queue(nullptr), m_task_handle(nullptr), m_port(0) {}

void AsyncNetLogger::init(const std::string &target_ip, uint16_t port) {
  m_ip = target_ip;
  m_port = port;
  if (m_log_queue == nullptr) {
    m_log_queue = xQueueCreate(20, sizeof(char *));
  }
}

void AsyncNetLogger::startWorker() {
  if (m_task_handle != nullptr)
    return;

  xTaskCreatePinnedToCore(&AsyncNetLogger::taskWrapper, "async_net_log_task",
                          4096, this, 2, &m_task_handle, 0);
}

void AsyncNetLogger::stopWorker() {
  if (m_task_handle != nullptr) {
    vTaskDelete(m_task_handle);
    m_task_handle = nullptr;
  }

  if (m_log_queue != nullptr) {
    char *leftover_str = nullptr;
    while (xQueueReceive(m_log_queue, &leftover_str, 0) == pdTRUE) {
      free(leftover_str);
    }
  }
}

bool AsyncNetLogger::queueLogMessage(const char *message) {
  if (m_task_handle == nullptr || m_log_queue == nullptr)
    return false;

  char *str_copy = strdup(message);
  if (str_copy == nullptr)
    return false;

  if (xQueueSend(m_log_queue, &str_copy, 0) != pdTRUE) {
    free(str_copy);
    return false;
  }
  return true;
}

void AsyncNetLogger::taskWrapper(void *pv) {
  static_cast<AsyncNetLogger *>(pv)->run();
}

void AsyncNetLogger::run() {
  int log_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (log_socket < 0) {
    vTaskDelete(NULL);
    return;
  }

  struct sockaddr_in dest_addr;
  dest_addr.sin_addr.s_addr = inet_addr(m_ip.c_str());
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(m_port);

  char *log_msg_ptr = nullptr;

  while (true) {
    if (xQueueReceive(m_log_queue, &log_msg_ptr, portMAX_DELAY) == pdTRUE) {
      if (log_msg_ptr != nullptr) {
        sendto(log_socket, log_msg_ptr, strlen(log_msg_ptr), 0,
               (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        free(log_msg_ptr);
      }
    }
  }

  if (log_socket >= 0)
    close(log_socket);
  vTaskDelete(NULL);
}
