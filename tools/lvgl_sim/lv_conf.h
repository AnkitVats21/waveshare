#pragma once

#define LV_CONF_INCLUDE_SIMPLE 1

/*------------------------
 * Color settings
 *-----------------------*/
#define LV_COLOR_DEPTH 32

/*------------------------
 * Memory / stdlib
 *-----------------------*/
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_STDINT_INCLUDE      <stdint.h>
#define LV_STDDEF_INCLUDE      <stddef.h>
#define LV_STDBOOL_INCLUDE     <stdbool.h>
#define LV_INTTYPES_INCLUDE    <inttypes.h>
#define LV_LIMITS_INCLUDE      <limits.h>
#define LV_STDARG_INCLUDE      <stdarg.h>

/*------------------------
 * Tick / OS
 *-----------------------*/
/* Tick source is wired at runtime via lv_tick_set_cb(SDL_GetTicks) in main.cpp
 * (LVGL v9 dropped the old LV_TICK_CUSTOM build-time macro). */
#define LV_USE_OS LV_OS_NONE

/*------------------------
 * Feature configuration
 *-----------------------*/
#define LV_USE_LOG 1
#if LV_USE_LOG
    #define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
    #define LV_LOG_PRINTF 1
#endif

#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0

#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR  0

/*------------------------
 * Widgets used by this tool
 *-----------------------*/
#define LV_USE_LABEL     1
#define LV_USE_BAR       1
#define LV_USE_SLIDER    1
#define LV_USE_BUTTON    1
#define LV_USE_IMAGE     1
#define LV_USE_ARC       1
#define LV_USE_SWITCH    1
#define LV_USE_TEXTAREA  1
#define LV_USE_TABLE     0
#define LV_USE_CHART     0
#define LV_USE_LIST      0
#define LV_USE_ANIMIMG   0
#define LV_USE_TILEVIEW  1

/*------------------------
 * Layouts
 *-----------------------*/
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/*------------------------
 * Themes
 *-----------------------*/
#define LV_USE_THEME_DEFAULT 1
#if LV_USE_THEME_DEFAULT
    #define LV_THEME_DEFAULT_DARK 0
    #define LV_THEME_DEFAULT_GROW 1
    #define LV_THEME_DEFAULT_TRANSITION_TIME 80
#endif
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO   0

/*------------------------
 * Fonts
 *-----------------------*/
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/*------------------------
 * Others
 *-----------------------*/
#define LV_USE_SYSMON  0
#define LV_USE_PROFILER 0
#define LV_USE_SNAPSHOT 0

/* stdio filesystem driver ("S:/path") + built-in JPEG decoder, used to show
 * album art thumbnails downloaded from the ESP's /api/files/download and
 * cached to a temp file on disk. */
#define LV_USE_FS_STDIO 1
#if LV_USE_FS_STDIO
    #define LV_FS_STDIO_LETTER 'S'
    #define LV_FS_STDIO_PATH ""
    #define LV_FS_STDIO_CACHE_SIZE 0
#endif
#define LV_USE_TJPGD 1

#define LV_USE_FS_POSIX 0
#define LV_USE_FS_WIN32 0
#define LV_USE_LIBPNG   0
#define LV_USE_LIBJPEG_TURBO 0
#define LV_USE_FREETYPE 0

#define LV_USE_DRAW_SW 1

/* LVGL's own SDL window+input driver -- handles the window, texture flush,
 * and mouse/keyboard event pump internally (see src/drivers/sdl). Using this
 * instead of hand-rolled SDL glue avoids re-implementing (and re-bugging)
 * flush buffering and input event handling ourselves. */
#define LV_USE_SDL 1
#if LV_USE_SDL
    #define LV_SDL_INCLUDE_PATH <SDL2/SDL.h>
    #define LV_SDL_RENDER_MODE LV_DISPLAY_RENDER_MODE_DIRECT
    #define LV_SDL_BUF_COUNT 1
    #define LV_SDL_ACCELERATED 1
    #define LV_SDL_FULLSCREEN 0
    #define LV_SDL_DIRECT_EXIT 1
#endif
