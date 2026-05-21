#pragma once

#include "RtpTaskBase.h"

class RtpReceiver : public RtpTaskBase {
public:
  RtpReceiver(const CommonConfig &config, RingbufHandle_t ring_buffer, int shared_socket, const char *log_tag)
      : RtpTaskBase(config, ring_buffer, shared_socket, log_tag), m_is_interrupted(false) {}

  void setInterrupted(bool interrupted) { m_is_interrupted = interrupted; }
  bool isInterrupted() const { return m_is_interrupted; }

protected:
  void processLoop() override;

private:
  volatile bool m_is_interrupted;
};
