#include "DashboardScreen.h"

#include "PlaceholderData.h"
#include "theme/Theme.h"

#include <cstdio>

namespace lvgl_sim::ui {

namespace {
namespace ph = placeholder;
} // namespace

DashboardScreen::DashboardScreen(ui_view::IUiDataSource& data_source)
    : m_data_source(data_source) {
    const int w = lv_display_get_horizontal_resolution(nullptr);
    const int h = lv_display_get_vertical_resolution(nullptr);
    const theme::Metrics metrics = theme::Metrics::forScreen(w, h);

    m_screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(m_screen, theme::kBg, 0);
    lv_obj_set_style_pad_all(m_screen, metrics.spacing_md, 0);
    lv_obj_set_flex_flow(m_screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(m_screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(m_screen, metrics.spacing_sm, 0);

    // ---- Topbar ----
    lv_obj_t* topbar = lv_obj_create(m_screen);
    lv_obj_set_size(topbar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(topbar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(topbar, 0, 0);
    lv_obj_set_style_pad_all(topbar, 0, 0);
    lv_obj_set_flex_flow(topbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(topbar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* status_group = lv_obj_create(topbar);
    lv_obj_set_size(status_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(status_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_group, 0, 0);
    lv_obj_set_style_pad_all(status_group, 0, 0);
    lv_obj_set_flex_flow(status_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_group, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_group, metrics.spacing_md, 0);

    // Tappable status icon replaces the dedicated Network page -- Wi-Fi
    // state is now visible on every page instead of needing a swipe to
    // check; tap the icon for detail.
    m_wifi_icon = lv_label_create(status_group);
    lv_obj_set_style_text_font(m_wifi_icon, metrics.font_lg, 0);
    lv_label_set_text(m_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_add_flag(m_wifi_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(m_wifi_icon, onWifiIconClicked, LV_EVENT_CLICKED, this);

    lv_obj_t* temp_label = lv_label_create(topbar);
    lv_obj_set_style_text_color(temp_label, theme::kTextMuted, 0);
    lv_obj_set_style_text_font(temp_label, metrics.font_lg, 0);
    // Placeholder -- no weather integration, see PlaceholderData.h.
    lv_label_set_text(temp_label, ph::kWeatherTempText);

    m_status_sheet.create(m_screen);

    const int content_width = w - 2 * metrics.spacing_md;
    // Reserve room for the topbar above the carousel.
    const int carousel_height = h - 2 * metrics.spacing_md - 24 - metrics.spacing_sm;

    m_carousel.create(m_screen, content_width, carousel_height > 60 ? carousel_height : 60);

    // ---- Music page ----
    lv_obj_t* music_page = m_carousel.addPage("Music");
    m_music_page.create(music_page, content_width, data_source);

    // ---- System page ----
    lv_obj_t* system_page = m_carousel.addPage("System");

    m_cpu_row.create(system_page, LV_SYMBOL_SETTINGS, "CPU", content_width);
    lv_obj_t* cpu_body = m_cpu_row.body();
    lv_obj_set_flex_flow(cpu_body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(cpu_body, metrics.spacing_sm, 0);
    m_cpu0_spark.create(cpu_body, 16, 4, 28, theme::kAccent);
    m_cpu1_spark.create(cpu_body, 16, 4, 28, theme::kAccent2);
    // Split the row evenly between the two cores instead of each sparkline
    // hugging its own bar width and leaving the rest of the row empty.
    lv_obj_set_width(m_cpu0_spark.row(), 0);
    lv_obj_set_flex_grow(m_cpu0_spark.row(), 1);
    lv_obj_set_width(m_cpu1_spark.row(), 0);
    lv_obj_set_flex_grow(m_cpu1_spark.row(), 1);

    lv_obj_t* mem_card = lv_obj_create(system_page);
    lv_obj_set_size(mem_card, content_width, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(mem_card, theme::kCard, 0);
    lv_obj_set_style_border_width(mem_card, 0, 0);
    lv_obj_set_style_radius(mem_card, 14, 0);
    lv_obj_set_style_pad_all(mem_card, 12, 0);
    m_memory_bar.create(mem_card, "Memory (heap + PSRAM)", content_width - 24);

    lv_obj_t* storage_card = lv_obj_create(system_page);
    lv_obj_set_size(storage_card, content_width, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(storage_card, theme::kCard, 0);
    lv_obj_set_style_border_width(storage_card, 0, 0);
    lv_obj_set_style_radius(storage_card, 14, 0);
    lv_obj_set_style_pad_all(storage_card, 12, 0);
    m_storage_bar.create(storage_card, "Storage (SD card)", content_width - 24);
    // Real values arrive from GET /api/storage/info, refreshed in update()
    // below -- this initial text is just "no data yet" until the first
    // slow-cadence poll lands.
    m_storage_bar.setValueText("--");

    // ---- Ambient page ----
    lv_obj_t* ambient_page = m_carousel.addPage("Ambient");
    m_weather_row.create(ambient_page, LV_SYMBOL_HOME, "Weather", content_width);
    // Placeholder -- no weather integration, see PlaceholderData.h.
    m_weather_row.setValueText(ph::kWeatherTempText);
    lv_obj_t* weather_body = m_weather_row.body();
    lv_obj_t* weather_sub = lv_label_create(weather_body);
    lv_obj_set_style_text_color(weather_sub, theme::kTextMuted, 0);
    lv_obj_set_style_text_font(weather_sub, metrics.font_sm, 0);
    lv_label_set_text(weather_sub, ph::kWeatherConditionText);

    m_assistant_row.create(ambient_page, LV_SYMBOL_CALL, "Assistant", content_width);
    m_assistant_row.setClickable(true);
    lv_obj_add_event_cb(m_assistant_row.container(), onAssistantRowClicked, LV_EVENT_CLICKED, this);
    // Placeholder -- model/voice not echoed into UiSnapshot yet.
    m_assistant_row.setValueText(LV_SYMBOL_RIGHT);
    lv_obj_t* assistant_body = m_assistant_row.body();
    lv_obj_t* assistant_sub = lv_label_create(assistant_body);
    lv_obj_set_style_text_color(assistant_sub, theme::kTextMuted, 0);
    lv_obj_set_style_text_font(assistant_sub, metrics.font_sm, 0);
    char assistant_buf[64];
    std::snprintf(assistant_buf, sizeof(assistant_buf), "%s / voice %s",
                  ph::kAssistantModelText, ph::kAssistantVoiceText);
    lv_label_set_text(assistant_sub, assistant_buf);

    // ---- Controls page ----
    lv_obj_t* controls_page = m_carousel.addPage("Controls");
    m_led_row.create(controls_page, LV_SYMBOL_IMAGE, "LED color", content_width);
    m_led_row.setClickable(true);
    m_led_row.setValueText(LV_SYMBOL_RIGHT);
    lv_obj_add_event_cb(m_led_row.container(), onLedRowClicked, LV_EVENT_CLICKED, this);
    lv_obj_t* led_body = m_led_row.body();
    m_led_swatch = lv_obj_create(led_body);
    lv_obj_set_size(m_led_swatch, 24, 24);
    lv_obj_set_style_radius(m_led_swatch, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(m_led_swatch, 0, 0);
    lv_obj_set_style_bg_color(m_led_swatch, m_led_color, 0);

    m_led_sheet.create(m_screen);
    m_led_sheet.setOnApply([this](const std::string& mode, lv_color_t color) {
        m_led_mode = mode;
        m_led_color = color;
        lv_obj_set_style_bg_color(m_led_swatch, color, 0);
        // Sent to the real WS2812 strip via POST /api/led/set -- update()
        // will re-sync the swatch/mode from the device's own read-back on
        // the next slow-cadence poll, confirming it actually took effect.
        m_data_source.setLed(mode, color.red, color.green, color.blue);
    });
}

void DashboardScreen::update(const ui_view::UiSnapshot& snap) {
    const auto& s = snap.system;

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d%% / %d%%", s.cpu0_pct, s.cpu1_pct);
    m_cpu_row.setValueText(buf);
    m_cpu0_spark.pushSample(s.cpu0_pct / 100.0f);
    m_cpu1_spark.pushSample(s.cpu1_pct / 100.0f);

    // Real totals from GET /api/system/init (heap_caps_get_total_size),
    // refreshed at the slow cadence in HttpUiDataSource::pollLoop -- 0
    // until the first fetch lands, so skip the percentage until then
    // rather than divide by zero or show a misleading number.
    const uint32_t total_capacity = s.heap_internal_total + s.heap_psram_total;
    const uint32_t total_free = s.heap_internal_free + s.heap_psram_free;
    if (total_capacity > 0) {
        const int used_pct = static_cast<int>(100 - (100ull * total_free / total_capacity));
        m_memory_bar.setPercent(used_pct);
    }
    std::snprintf(buf, sizeof(buf), "%lu KB free",
                  static_cast<unsigned long>(total_free / 1024));
    m_memory_bar.setValueText(buf);

    // Real values from GET /api/storage/info.
    if (snap.storage.mounted && snap.storage.total_bytes > 0) {
        const uint64_t used = snap.storage.total_bytes - snap.storage.free_bytes;
        const int storage_pct = static_cast<int>(100ull * used / snap.storage.total_bytes);
        m_storage_bar.setPercent(storage_pct);
        std::snprintf(buf, sizeof(buf), "%.1f GB / %.1f GB",
                      used / (1024.0 * 1024.0 * 1024.0),
                      snap.storage.total_bytes / (1024.0 * 1024.0 * 1024.0));
        m_storage_bar.setValueText(buf);
    } else {
        m_storage_bar.setValueText("Not mounted");
    }

    // Real LED state read back from GET /api/system/init -- keeps the
    // swatch/mode honest if it changed some other way (e.g. a future
    // ambient/assistant-driven LED mode), not just after our own Apply.
    const char* mode_names[] = {"off", "solid", "blink", "breath", "rainbow"};
    const std::string device_mode = mode_names[static_cast<int>(snap.led.mode)];
    const lv_color_t device_led = LV_COLOR_MAKE(snap.led.r, snap.led.g, snap.led.b);
    if (device_mode != m_led_mode || snap.led.r != m_led_color.red ||
        snap.led.g != m_led_color.green || snap.led.b != m_led_color.blue) {
        m_led_mode = device_mode;
        m_led_color = device_led;
        lv_obj_set_style_bg_color(m_led_swatch, device_led, 0);
    }

    if (s.wifi_connected) {
        std::snprintf(buf, sizeof(buf), "%s (%d dBm)", s.wifi_ssid.c_str(), s.wifi_rssi);
        m_wifi_detail_text = buf;
        lv_obj_set_style_text_color(m_wifi_icon, theme::kGood, 0);
    } else {
        m_wifi_detail_text = "Disconnected";
        lv_obj_set_style_text_color(m_wifi_icon, theme::kBad, 0);
    }

    if (!snap.data_source_online) {
        m_wifi_detail_text = "Device unreachable";
        lv_obj_set_style_text_color(m_wifi_icon, theme::kBad, 0);
    }

    m_music_page.update(snap);
}

void DashboardScreen::setOnOpenAssistant(std::function<void()> cb) {
    m_on_open_assistant = std::move(cb);
}

void DashboardScreen::onAssistantRowClicked(lv_event_t* e) {
    auto* self = static_cast<DashboardScreen*>(lv_event_get_user_data(e));
    if (self->m_on_open_assistant) self->m_on_open_assistant();
}

void DashboardScreen::onLedRowClicked(lv_event_t* e) {
    auto* self = static_cast<DashboardScreen*>(lv_event_get_user_data(e));
    self->m_led_sheet.setCurrentState(self->m_led_mode, self->m_led_color);
    self->m_led_sheet.open();
}

void DashboardScreen::onWifiIconClicked(lv_event_t* e) {
    auto* self = static_cast<DashboardScreen*>(lv_event_get_user_data(e));
    self->m_status_sheet.show("Wi-Fi", self->m_wifi_detail_text);
}

} // namespace lvgl_sim::ui
