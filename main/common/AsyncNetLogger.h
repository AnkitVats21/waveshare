#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string>

/**
 * @brief Asynchronously sends log messages over the network via UDP
 */
class AsyncNetLogger {
public:
  static AsyncNetLogger &getInstance();

  /**
   * @brief Initialize the logger with target IP and port
   */
  void init(const std::string &target_ip, uint16_t port);

  /**
   * @brief Start the background worker task
   */
  void startWorker();

  /**
   * @brief Stop the background worker task and clean up
   */
  void stopWorker();

  /**
   * @brief Queue a log message for network transmission
   * @param message The log message to queue
   * @return true if successfully queued
   */
  bool queueLogMessage(const char *message);

private:
  AsyncNetLogger();

  QueueHandle_t m_log_queue;
  TaskHandle_t m_task_handle;
  std::string m_ip;
  uint16_t m_port;

  static void taskWrapper(void *pv);
  void run();
};
