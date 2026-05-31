#pragma once

#include "services/BufferManager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * @brief Base class for RTP network tasks (Streamer + Receiver).
 *
 * Holds a BufferManager::BufferId instead of a raw RingbufHandle_t.
 * All buffer I/O goes through BufferManager::send/receive/returnItem/flush.
 */
class RtpTaskBase {
public:
  struct CommonConfig {
    uint16_t port;
    uint8_t priority;
    uint32_t stack_size;
    BaseType_t core_id;
  };

  /**
   * @param buf_id   BufferManager ID for the ring buffer this task reads/writes.
   * @param shared_socket  ≥0 = externally owned UDP socket (not closed on stop).
   */
  RtpTaskBase(const CommonConfig &config, BufferManager::BufferId buf_id,
              int shared_socket, const char *log_tag);

  virtual ~RtpTaskBase();

  bool begin();

  void stop();

  void setEnabled(bool enabled) { m_is_enabled = enabled; }

protected:
  CommonConfig           m_config;
  BufferManager::BufferId m_buf_id;      ///< Replaces raw m_ring_buffer
  TaskHandle_t           m_task_handle;
  int                    m_socket;
  bool                   m_is_shared_socket;
  const char            *m_tag;
  volatile bool          m_is_running;
  volatile bool          m_is_enabled;

  // Pure virtual: derived classes implement their specific loop
  virtual void processLoop() = 0;

  void closeSocket();

private:
  static void taskWrapper(void *pvParameters);
};
