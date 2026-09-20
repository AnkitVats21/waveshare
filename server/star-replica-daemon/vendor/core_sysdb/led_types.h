#pragma once

/**
 * @brief LED color and animation types.
 *
 * Includes all LED-related enums, structs, and preset color macros.
 * Used by LedService (app layer) and AppController for status feedback.
 */

#include <cstdint>

enum class LedMode : uint8_t { OFF, SOLID, BLINK, BREATH, RAINBOW };

struct RgbColor {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

struct LedEventData {
  LedMode  mode;
  RgbColor color;
  uint32_t speed_ms;
  uint8_t  repeat_count; ///< 0 = infinite / not applicable
};

// ---------------------------------------------------------------------------
// Preset colors (GRB wiring on WS2812 — swap R and G in the struct)
// ---------------------------------------------------------------------------
// NOTE: The WS2812 on this board is wired GRB. The struct fields are named
// {r, g, b} but the hardware interprets them as {G, R, B}.
// So to display RED you set g=80, r=0, b=0 (G slot carries the red value).
#define RED_LED    RgbColor{0,   80,  0}
#define GREEN_LED  RgbColor{80,  0,   0}
#define BLUE_LED   RgbColor{0,   0,   80}
#define YELLOW_LED RgbColor{80,  80,  0}
#define PURPLE_LED RgbColor{160, 88,  0}
#define ORANGE_LED RgbColor{80,  40,  0}
#define PINK_LED   RgbColor{80,  0,   40}
#define BROWN_LED  RgbColor{80,  60,  0}
#define WHITE_LED  RgbColor{80,  80,  80}
#define OFF_LED    RgbColor{0,   0,   0}
