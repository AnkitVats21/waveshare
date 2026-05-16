#pragma once

#include "RtpTaskBase.h"
#include <arpa/inet.h>
#include <sys/socket.h>

class RtpReceiver : public RtpTaskBase {
public:
  using RtpTaskBase::RtpTaskBase;

protected:
  void processLoop() override;

private:
  int m_socket = -1;
  void closeSocket();
};
