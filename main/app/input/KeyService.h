#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/input/ExpanderKeyInput.h"

class KeyService {
public:
  explicit KeyService(ExpanderKeyInput &input);

  void start();

private:
  ExpanderKeyInput &m_input;

  static void taskEntry(void *arg);
  void taskLoop();

  bool m_prevState[5] = {false};

  static constexpr const char *TAG = "KeyService";
};