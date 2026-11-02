#pragma once

#include <cstdint>

class LedStripManager;

/**
 * @brief Deep sleep in place of the power switch (KEY_2).
 *
 * Holding KEY_2 for SLEEP_HOLD_MS (KeyService) sends the board to deep sleep
 * once the key is released. KEY_2 is GPIO0, an RTC pin: pressing it wakes the
 * chip, which boots and keeps going only if the key is still held
 * WAKE_HOLD_MS; a tap goes straight back to sleep. A timer also wakes the
 * board ALARM_LEAD_S before the next alarm, timer or reminder, which then
 * rings as usual and the board stays on.
 */
namespace Services::DeepSleep {

constexpr uint32_t SLEEP_HOLD_MS = 5000;
constexpr uint32_t WAKE_HOLD_MS = 2000;
constexpr int64_t ALARM_LEAD_S = 60;

// Early in boot, with the LED strip up: after a KEY_2 wake, waits for the
// hold (the LEDs fill) or goes back to sleep. True when the board woke by
// KEY_2 and stays on.
bool checkWake(LedStripManager& leds);

// Stops recording and music, turns the amplifier, backlight, LEDs and Wi-Fi
// off and sleeps until KEY_2 or the next alarm. Call with the key released.
[[noreturn]] void enter();

} // namespace Services::DeepSleep
