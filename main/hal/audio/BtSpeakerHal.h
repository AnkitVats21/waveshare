#pragma once

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/i2s_types.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include <cstdint>
#include <cstddef>

/**
 * @brief HAL driver for Bluetooth Speaker Daughter Board I2S Audio Output.
 *
 * Owns and manages a dedicated I2S TX master channel on I2S_NUM_0 to output
 * 16-bit PCM audio (at 44.1 kHz stereo) over GPIO pins 4, 5, 6, 7 to the ESP32 daughter board.
 */
class BtSpeakerHal {
public:
    struct Config {
        gpio_num_t bck_pin = GPIO_NUM_4;
        gpio_num_t ws_pin = GPIO_NUM_5;
        gpio_num_t dout_pin = GPIO_NUM_6;
        gpio_num_t mclk_pin = GPIO_NUM_7;
        uint32_t sample_rate = 48000;
        int port = I2S_NUM_0;
    };

    BtSpeakerHal() = default;
    ~BtSpeakerHal();

    esp_err_t init();
    esp_err_t init(const Config& cfg);
    esp_err_t deinit();

    esp_err_t writePcm(const void* data, size_t bytes, size_t* bytes_written, uint32_t timeout_ms = portMAX_DELAY);

    bool isInitialized() const { return m_initialized; }
    i2s_chan_handle_t getTxHandle() const { return m_tx_handle; }
    uint32_t getSampleRate() const { return m_config.sample_rate; }

private:
    i2s_chan_handle_t m_tx_handle = nullptr;
    Config m_config;
    bool m_initialized = false;

    static constexpr const char* TAG = "BtSpeakerHal";
};
