#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

class RtpTaskBase {
public:
  struct CommonConfig {
    uint16_t port;
    uint8_t priority;
    uint32_t stack_size;
    BaseType_t core_id;
  };

  RtpTaskBase(const CommonConfig &config, RingbufHandle_t ring_buffer,
              const char *log_tag);

  virtual ~RtpTaskBase();

  bool begin();

  void stop();

protected:
  CommonConfig m_config;
  RingbufHandle_t m_ring_buffer;
  TaskHandle_t m_task_handle;
  int m_socket;
  const char *m_tag;
  volatile bool m_is_running;

  // Pure virtual method: Derived classes implement their specific logic here
  virtual void processLoop() = 0;

  void closeSocket();

private:
  static void taskWrapper(void *pvParameters);
};
