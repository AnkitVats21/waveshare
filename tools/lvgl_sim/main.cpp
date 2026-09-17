#include "HttpUiDataSource.h"
#include "ui/AssistantScreen.h"
#include "ui/DashboardScreen.h"

#include <lvgl.h>
#include <src/drivers/sdl/lv_sdl_window.h>
#include <src/drivers/sdl/lv_sdl_mouse.h>
#include <SDL2/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

// Simulator resolution. Defaults to the real touchscreen panel spec
// (320x240 landscape) per the approved widget-redesign plan -- widgets
// size themselves off theme::Metrics rather than pixel literals, so this
// pair can be bumped back up for comfortable desktop viewing without
// touching any widget code.
constexpr int kScreenWidth = 320;
constexpr int kScreenHeight = 240;
constexpr int kDefaultPort = 80;

enum class ActiveScreen {
    Dashboard,
    Assistant,
};

void printUsage(const char* argv0) {
    std::fprintf(stderr, "Usage: %s --host <esp-ip> [--port <port>]\n", argv0);
}

} // namespace

int main(int argc, char** argv) {
    std::string host;
    int port = kDefaultPort;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return 0;
        }
    }

    if (host.empty()) {
        const char* env_host = std::getenv("ESP_HOST");
        if (env_host) host = env_host;
    }
    if (host.empty()) {
        printUsage(argv[0]);
        return 1;
    }

    lv_init();

    // LVGL's own SDL window/input driver: owns the window, renderer, flush
    // buffering, and SDL event pump (mouse/keyboard) internally -- see
    // src/drivers/sdl in the vendored LVGL source.
    lv_display_t* display = lv_sdl_window_create(kScreenWidth, kScreenHeight);
    lv_sdl_window_set_title(display, "lvgl_sim");
    lv_sdl_mouse_create();

    lvgl_sim::HttpUiDataSource data_source(host, static_cast<uint16_t>(port));

    lvgl_sim::ui::DashboardScreen dashboard(data_source);
    lvgl_sim::ui::AssistantScreen assistant;

    lv_obj_t* active_screen = dashboard.root();
    lv_screen_load(active_screen);

    auto go_to = [&](ActiveScreen target) {
        switch (target) {
            case ActiveScreen::Dashboard:  active_screen = dashboard.root(); break;
            case ActiveScreen::Assistant:  active_screen = assistant.root(); break;
        }
        lv_screen_load(active_screen);
    };

    dashboard.setOnOpenAssistant([&]() { go_to(ActiveScreen::Assistant); });
    assistant.setOnBack([&]() { go_to(ActiveScreen::Dashboard); });

    while (true) {
        ui_view::UiSnapshot snap = data_source.getSnapshot();
        dashboard.update(snap);
        assistant.update(snap);

        const uint32_t next_ms = lv_timer_handler();
        SDL_Delay(next_ms > 0 && next_ms < 20 ? next_ms : 5);
    }
}
