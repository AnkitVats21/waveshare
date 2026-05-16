#pragma once

#include "RtpTaskBase.h"

class RtpReceiver : public RtpTaskBase {
public:
  using RtpTaskBase::RtpTaskBase;

protected:
  void processLoop() override;
};
