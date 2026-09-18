#pragma once

#include "hal/Board_defs.h"
#include "esp_err.h"
#include <memory>

#if CONFIG_DISPLAY_ENABLE
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "ui_view/SysDbUiDataSource.h"
#include "DashboardScreen.h"
#include "AssistantScreen.h"

#if CONFIG_TOUCH_ENABLE
#include "esp_lcd_touch.h"
#endif

class LcdManager {
public:
    static LcdManager& getInstance();

    esp_err_t begin();
    bool isInitialized() const { return m_initialized; }

    void tick();

private:
    LcdManager() = default;
    ~LcdManager() = default;
    LcdManager(const LcdManager&) = delete;
    LcdManager& operator=(const LcdManager&) = delete;

    enum class ActiveScreen {
        Dashboard,
        Assistant,
    };

    bool initSpiBus();
    bool initLcdPanel();
#if CONFIG_TOUCH_ENABLE
    bool initTouch();
#endif
    bool initLvgl();

    static void timerCallback(lv_timer_t* timer);

    bool m_initialized = false;
    esp_lcd_panel_io_handle_t m_io_handle = nullptr;
    esp_lcd_panel_handle_t    m_panel_handle = nullptr;
    lv_display_t*             m_display = nullptr;

#if CONFIG_TOUCH_ENABLE
    esp_lcd_touch_handle_t    m_touch_handle = nullptr;
    lv_indev_t*               m_touch_indev = nullptr;
#endif

    ui_view::SysDbUiDataSource m_data_source;
    std::unique_ptr<lvgl_sim::ui::DashboardScreen> m_dashboard;
    std::unique_ptr<lvgl_sim::ui::AssistantScreen> m_assistant;
    ActiveScreen m_active_screen = ActiveScreen::Dashboard;
    lv_timer_t* m_update_timer = nullptr;

    static constexpr const char* TAG = "LcdManager";
};

#endif // CONFIG_DISPLAY_ENABLE
