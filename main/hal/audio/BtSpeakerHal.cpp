#include "hal/audio/BtSpeakerHal.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

BtSpeakerHal::~BtSpeakerHal() {
    deinit();
}

esp_err_t BtSpeakerHal::init() {
    return init(Config{});
}

esp_err_t BtSpeakerHal::init(const Config& cfg) {
    if (m_initialized) {
        ESP_LOGI(TAG, "BtSpeakerHal already initialized");
        return ESP_OK;
    }
    m_config = cfg;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(m_config.port, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    esp_err_t ret = i2s_new_channel(&chan_cfg, &m_tx_handle, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate I2S TX channel: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(m_config.sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = m_config.mclk_pin,
            .bclk = m_config.bck_pin,
            .ws = m_config.ws_pin,
            .dout = m_config.dout_pin,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(m_tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S STD mode: %s", esp_err_to_name(ret));
        i2s_del_channel(m_tx_handle);
        m_tx_handle = nullptr;
        return ret;
    }

    ret = i2s_channel_enable(m_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S TX channel: %s", esp_err_to_name(ret));
        i2s_del_channel(m_tx_handle);
        m_tx_handle = nullptr;
        return ret;
    }

    m_initialized = true;
    ESP_LOGI(TAG, "BtSpeakerHal initialized successfully at %lu Hz (BCK:%d, WS:%d, DOUT:%d, MCLK:%d)",
             (unsigned long)m_config.sample_rate, m_config.bck_pin, m_config.ws_pin,
             m_config.dout_pin, m_config.mclk_pin);
    return ESP_OK;
}

esp_err_t BtSpeakerHal::deinit() {
    if (!m_initialized) {
        return ESP_OK;
    }
    if (m_tx_handle) {
        i2s_channel_disable(m_tx_handle);
        i2s_del_channel(m_tx_handle);
        m_tx_handle = nullptr;
    }
    m_initialized = false;
    ESP_LOGI(TAG, "BtSpeakerHal deinitialized");
    return ESP_OK;
}

esp_err_t BtSpeakerHal::writePcm(const void* data, size_t bytes, size_t* bytes_written, uint32_t timeout_ms) {
    if (!m_initialized || !m_tx_handle) {
        return ESP_FAIL;
    }
    TickType_t ticks = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return i2s_channel_write(m_tx_handle, data, bytes, bytes_written, ticks);
}
