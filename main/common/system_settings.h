#pragma once

#include "freertos/FreeRTOS.h"
#include <cstdint>
#include <string>

/**
 * @brief Audio stream format selector.
 */
enum class AudioStreamFormat {
  PCM_S16LE, ///< Raw 16-bit signed little-endian PCM
  G711_ULAW  ///< 8-bit u-law compressed
};

/**
 * @brief System-wide configuration loaded from Kconfig (and overridable via NVS).
 *
 * This struct is the single source of configuration truth for the entire
 * application. It is owned by SystemContext and accessed via SystemContext::get().
 *
 * Wi-Fi credentials and server IPs are populated from Kconfig defaults
 * (see Kconfig.projbuild) so they never appear as hard-coded strings in .cpp files.
 */
struct GlobalSystemSettings {
  // WiFi
  std::string wifi_ssid;
  std::string wifi_password;
  int         wifi_max_retries = 5;

  // Network / streaming
  std::string       server_ip     = "192.168.1.18";
  uint16_t          tx_rtp_port   = 5005;
  uint16_t          rx_rtp_port   = 5005;
  AudioStreamFormat stream_format = AudioStreamFormat::PCM_S16LE;
  uint32_t          buffer_size   = 131072; ///< PSRAM ring buffer size (128KB)

  // Audio hardware
  uint32_t sample_rate   = 16000;
  bool     mic_enabled   = true;

  // FreeRTOS task tuning
  BaseType_t network_core_id    = 0;
  uint8_t    tx_priority        = 5;
  uint8_t    rx_priority        = 6;
  uint8_t    audio_task_priority = 22;
  uint32_t   audio_stack_size   = 8192;
  BaseType_t audio_core_id      = 1;
};
