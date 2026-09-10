#include "BtPlayerI2s.h"
#include "esp_log.h"
#include <cstring>
#include <algorithm>

namespace btplayer {

BtPlayerI2s& BtPlayerI2s::getInstance() {
    static BtPlayerI2s instance;
    return instance;
}

BtPlayerI2s::~BtPlayerI2s() {
    deinit();
}

esp_err_t BtPlayerI2s::init(const Config& config) {
    if (m_initialized) {
        ESP_LOGW(TAG, "BtPlayerI2s already initialized");
        return ESP_OK;
    }

    std::memset(m_silence_buf, 0, sizeof(m_silence_buf));

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(config.port, I2S_ROLE_MASTER);
    chan_cfg.auto_clear    = true;
    chan_cfg.dma_desc_num  = config.dma_desc_num;
    chan_cfg.dma_frame_num = config.dma_frame_num;

    esp_err_t ret = i2s_new_channel(&chan_cfg, &m_tx_handle, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(config.sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = static_cast<gpio_num_t>(config.bclk_pin),
            .ws   = static_cast<gpio_num_t>(config.ws_pin),
            .dout = static_cast<gpio_num_t>(config.dout_pin),
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(m_tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(ret));
        i2s_del_channel(m_tx_handle);
        m_tx_handle = nullptr;
        return ret;
    }

    ret = i2s_channel_enable(m_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(ret));
        i2s_del_channel(m_tx_handle);
        m_tx_handle = nullptr;
        return ret;
    }

    m_initialized = true;
    ESP_LOGI(TAG, "BtPlayerI2s master initialized on port %d (BCLK=%d, WS=%d, DOUT=%d @ %lu Hz 16-bit Stereo)",
             config.port, config.bclk_pin, config.ws_pin, config.dout_pin, config.sample_rate);

    // Prime DMA pipeline with initial silence so clock starts ticking immediately
    writeSilence(512);

    return ESP_OK;
}

void BtPlayerI2s::deinit() {
    if (!m_initialized) return;

    if (m_tx_handle != nullptr) {
        i2s_channel_disable(m_tx_handle);
        i2s_del_channel(m_tx_handle);
        m_tx_handle = nullptr;
    }

    m_initialized = false;
    ESP_LOGI(TAG, "BtPlayerI2s deinitialized");
}

size_t BtPlayerI2s::writeSamples(const int16_t* stereo_pcm, size_t frame_count, uint32_t timeout_ms) {
    if (!m_initialized || m_tx_handle == nullptr || stereo_pcm == nullptr || frame_count == 0) {
        return 0;
    }

    size_t bytes_to_write = frame_count * 2 * sizeof(int16_t);
    size_t total_written  = 0;
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(stereo_pcm);

    while (total_written < bytes_to_write) {
        size_t chunk_written = 0;
        esp_err_t ret = i2s_channel_write(m_tx_handle, ptr + total_written, bytes_to_write - total_written, &chunk_written, timeout_ms);
        total_written += chunk_written;
        if (ret != ESP_OK || chunk_written == 0) {
            if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "i2s_channel_write error: %s", esp_err_to_name(ret));
            }
            break;
        }
    }

    return total_written / (2 * sizeof(int16_t));
}

size_t BtPlayerI2s::writeSilence(size_t frame_count, uint32_t timeout_ms) {
    if (!m_initialized || m_tx_handle == nullptr || frame_count == 0) {
        return 0;
    }

    size_t total_written = 0;
    while (total_written < frame_count) {
        size_t chunk = std::min(frame_count - total_written, SILENCE_CHUNK_FRAMES);
        size_t written = writeSamples(m_silence_buf, chunk, timeout_ms);
        total_written += written;
        if (written < chunk) {
            break; // Timeout or buffer full
        }
    }
    return total_written;
}

} // namespace btplayer
