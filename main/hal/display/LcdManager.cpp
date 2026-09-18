#include "hal/display/LcdManager.h"

#if CONFIG_DISPLAY_ENABLE
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_log.h"

#if CONFIG_TOUCH_ENABLE
#include "esp_lcd_touch_xpt2046.h"
#endif

LcdManager& LcdManager::getInstance() {
    static LcdManager instance;
    return instance;
}

esp_err_t LcdManager::begin() {
    if (m_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing Waveshare SPI Display & Touch (ILI9341 + XPT2046)...");

    if (!initSpiBus()) {
        ESP_LOGE(TAG, "SPI bus initialization failed");
        return ESP_FAIL;
    }

    if (!initLcdPanel()) {
        ESP_LOGE(TAG, "LCD panel initialization failed");
        return ESP_FAIL;
    }

#if CONFIG_TOUCH_ENABLE
    if (!initTouch()) {
        ESP_LOGW(TAG, "Touch initialization failed (display will function without touch)");
    }
#endif

    if (!initLvgl()) {
        ESP_LOGE(TAG, "LVGL port initialization failed");
        return ESP_FAIL;
    }

    m_initialized = true;
    ESP_LOGI(TAG, "Waveshare SPI Display & Touch ready.");
    return ESP_OK;
}

bool LcdManager::initSpiBus() {
    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num     = LCD_GPIO_SCLK;
    buscfg.mosi_io_num     = LCD_GPIO_MOSI;
    buscfg.miso_io_num     = -1; // Display is transmit-only
    buscfg.quadwp_io_num   = -1;
    buscfg.quadhd_io_num   = -1;
    buscfg.max_transfer_sz = LCD_H_RES * 40 * sizeof(uint16_t);

    esp_err_t ret = spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(TAG, "SPI bus initialized successfully on SCLK=%d, MOSI=%d", LCD_GPIO_SCLK, LCD_GPIO_MOSI);
    return true;
}

bool LcdManager::initLcdPanel() {
    // 1. Panel IO (SPI) - 10 MHz conservative clock for jumper wire reliability
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.dc_gpio_num       = static_cast<gpio_num_t>(LCD_GPIO_DC);
    io_config.cs_gpio_num       = static_cast<gpio_num_t>(LCD_GPIO_CS);
    io_config.pclk_hz           = 10 * 1000 * 1000; // 10 MHz
    io_config.lcd_cmd_bits      = 8;
    io_config.lcd_param_bits    = 8;
    io_config.spi_mode          = 0;
    io_config.trans_queue_depth = 10;

    esp_err_t ret = esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(LCD_SPI_HOST), &io_config, &m_io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(ret));
        return false;
    }

    // 2. Panel Driver (ILI9341)
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = static_cast<gpio_num_t>(LCD_GPIO_RST);
    panel_config.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_config.data_endian    = LCD_RGB_DATA_ENDIAN_BIG;
    panel_config.bits_per_pixel = 16;

    ret = esp_lcd_new_panel_ili9341(m_io_handle, &panel_config, &m_panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_ili9341 failed: %s", esp_err_to_name(ret));
        return false;
    }

    esp_lcd_panel_reset(m_panel_handle);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_lcd_panel_init(m_panel_handle);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_lcd_panel_disp_on_off(m_panel_handle, true);
    esp_lcd_panel_invert_color(m_panel_handle, true); // Whitish high-contrast theme

    ESP_LOGI(TAG, "ILI9341 panel initialized successfully on CS=%d, DC=%d", LCD_GPIO_CS, LCD_GPIO_DC);

    // Optional backlight control
#if defined(LCD_GPIO_BL) && (LCD_GPIO_BL >= 0)
    gpio_config_t bl_conf = {};
    bl_conf.pin_bit_mask = 1ULL << LCD_GPIO_BL;
    bl_conf.mode         = GPIO_MODE_OUTPUT;
    gpio_config(&bl_conf);
    gpio_set_level(static_cast<gpio_num_t>(LCD_GPIO_BL), 1);
#endif

    return true;
}

#if CONFIG_TOUCH_ENABLE
bool LcdManager::initTouch() {
    esp_lcd_panel_io_handle_t tp_io_handle = nullptr;
    esp_lcd_panel_io_spi_config_t tp_io_config = {};
    tp_io_config.cs_gpio_num       = static_cast<gpio_num_t>(TOUCH_GPIO_CS);
    tp_io_config.dc_gpio_num       = GPIO_NUM_NC;
    tp_io_config.spi_mode          = 0;
    tp_io_config.pclk_hz           = 1 * 1000 * 1000;
    tp_io_config.trans_queue_depth = 3;
    tp_io_config.lcd_cmd_bits      = 8;
    tp_io_config.lcd_param_bits    = 8;

    esp_err_t ret = esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(LCD_SPI_HOST), &tp_io_config, &tp_io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create touch panel IO: %s", esp_err_to_name(ret));
        return false;
    }

    esp_lcd_touch_config_t tp_cfg = {};
    tp_cfg.x_max            = LCD_H_RES;
    tp_cfg.y_max            = LCD_V_RES;
    tp_cfg.rst_gpio_num     = GPIO_NUM_NC;
    tp_cfg.int_gpio_num     = static_cast<gpio_num_t>(TOUCH_GPIO_IRQ);
    tp_cfg.levels.reset     = 0;
    tp_cfg.levels.interrupt = 0;
    tp_cfg.flags.swap_xy    = 1;
    tp_cfg.flags.mirror_x   = 0;
    tp_cfg.flags.mirror_y   = 1;

    ret = esp_lcd_touch_new_spi_xpt2046(tp_io_handle, &tp_cfg, &m_touch_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_touch_new_spi_xpt2046 failed: %s", esp_err_to_name(ret));
        return false;
    }
    return true;
}
#endif

bool LcdManager::initLvgl() {
    // Core 0 affinity: Audio DSP & I2S DMA on Core 1 are completely isolated
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority   = 4;
    port_cfg.task_stack      = 8192;
    port_cfg.task_stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    port_cfg.task_affinity   = 0;

    esp_err_t ret = lvgl_port_init(&port_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Add Display
    lvgl_port_display_cfg_t disp_cfg = {};
    disp_cfg.io_handle     = m_io_handle;
    disp_cfg.panel_handle  = m_panel_handle;
    disp_cfg.buffer_size   = LCD_H_RES * 10; // 10-line partial buffer in internal DMA SRAM (6.4 KB)
    disp_cfg.double_buffer = true;
    disp_cfg.hres          = LCD_H_RES;
    disp_cfg.vres          = LCD_V_RES;
    disp_cfg.monochrome    = false;
    disp_cfg.color_format  = LV_COLOR_FORMAT_RGB565;
    disp_cfg.rotation.swap_xy  = false;
    disp_cfg.rotation.mirror_x = true;  // Un-mirrors horizontal text in LVGL
    disp_cfg.rotation.mirror_y = false;
    disp_cfg.flags.buff_dma    = 1;
    disp_cfg.flags.buff_spiram = 0; // DMA capable requires internal SRAM on ESP32-S3 heap
    disp_cfg.flags.swap_bytes  = 1;

    m_display = lvgl_port_add_disp(&disp_cfg);
    if (!m_display) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return false;
    }
    esp_lcd_panel_invert_color(m_panel_handle, true); // Preserve whitish high-contrast theme
    ESP_LOGI(TAG, "LVGL display driver registered successfully.");

#if CONFIG_TOUCH_ENABLE
    if (m_touch_handle) {
        lvgl_port_touch_cfg_t touch_cfg = {};
        touch_cfg.disp   = m_display;
        touch_cfg.handle = m_touch_handle;
        m_touch_indev    = lvgl_port_add_touch(&touch_cfg);
    }
#endif

    // Build screens and navigation inside LVGL port lock
    if (lvgl_port_lock(0)) {
        m_dashboard = std::make_unique<lvgl_sim::ui::DashboardScreen>(m_data_source);
        m_assistant = std::make_unique<lvgl_sim::ui::AssistantScreen>();

        lv_screen_load(m_dashboard->root());

        m_dashboard->setOnOpenAssistant([this]() {
            m_active_screen = ActiveScreen::Assistant;
            lv_screen_load(m_assistant->root());
        });

        m_assistant->setOnBack([this]() {
            m_active_screen = ActiveScreen::Dashboard;
            lv_screen_load(m_dashboard->root());
        });

        // 4Hz UI refresh timer inside LVGL task context
        m_update_timer = lv_timer_create(timerCallback, 250, this);

        lvgl_port_unlock();
    }

    return true;
}

void LcdManager::timerCallback(lv_timer_t* timer) {
    auto* mgr = static_cast<LcdManager*>(lv_timer_get_user_data(timer));
    if (mgr) {
        mgr->tick();
    }
}

void LcdManager::tick() {
    ui_view::UiSnapshot snap = m_data_source.getSnapshot();
    if (m_active_screen == ActiveScreen::Dashboard && m_dashboard) {
        m_dashboard->update(snap);
    } else if (m_active_screen == ActiveScreen::Assistant && m_assistant) {
        m_assistant->update(snap);
    }
}

#endif // CONFIG_DISPLAY_ENABLE
