#pragma once

#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include <string>

/**
 * @brief Handles for physical audio hardware channels
 */
struct HardwareAudioHandles {
  i2s_chan_handle_t mic_rx_handle = nullptr;
  i2s_chan_handle_t speaker_tx_handle = nullptr;
  esp_codec_dev_handle_t play_dev = nullptr;
  esp_codec_dev_handle_t record_dev = nullptr;
};

enum class AudioStreamFormat {
  PCM_S16LE, // Raw 16-bit PCM
  G711_ULAW  // 8-bit u-law compression
};

struct RgbColor {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

enum class LedMode : uint8_t { OFF, SOLID, BLINK, BREATH, RAINBOW };

struct LedEventData {
  LedMode mode;
  RgbColor color;
  uint32_t speed_ms;
  uint8_t repeat_count; // 0 for infinite/not applicable
};

/**
 * @brief Unified Configuration Data Object
 */
struct GlobalSystemSettings {
  // WiFi Settings
  std::string wifi_ssid;
  std::string wifi_password;
  int wifi_max_retries = 5;

  // Stream Configuration
  AudioStreamFormat stream_format = AudioStreamFormat::PCM_S16LE;
  uint32_t sample_rate = 16000;
  bool mic_enabled = true;

  // Network Settings
  std::string server_ip = "192.168.1.18";
  uint16_t tx_rtp_port = 5005;
  uint16_t rx_rtp_port = 5006;

  // Task/Resource Settings
  BaseType_t network_core_id = 0;
  uint8_t tx_priority = 5;
  uint8_t rx_priority = 6;
  uint32_t buffer_size = 131072; // 128KB PSRAM buffer enabled

  uint8_t audio_task_priority = 22;
  uint32_t audio_stack_size = 8192;
  BaseType_t audio_core_id = 1;
};

/**
 * @brief Unified Memory Handle Context Object
 */
struct GlobalPipelineContext {
  RingbufHandle_t tx_ring_buffer = nullptr;
  RingbufHandle_t rx_ring_buffer = nullptr;
};

/**
 * @brief System Event Bases
 */
ESP_EVENT_DECLARE_BASE(AUDIO_SYSTEM_EVENTS);
ESP_EVENT_DECLARE_BASE(APP_EVENTS);
ESP_EVENT_DECLARE_BASE(WIFI_SYSTEM_EVENTS);

/**
 * @brief Audio/Application Events
 */
enum class AppEvent : uint32_t {
  WAKE_WORD_DETECTED,
  STREAMING_STOP_REQUESTED,
  STOP_STREAMING,
  LED_COLOR_UPDATE,
  MIC_GAIN_UPDATE,
  LED_COMMAND
};

/**
 * @brief WiFi System Events
 */
enum class WifiEvent : uint32_t {
  CONNECTED,
  DISCONNECTED,
};
