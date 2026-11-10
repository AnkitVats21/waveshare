#pragma once

#include <cstdint>

namespace Input {

/**
 * @brief One key's press, click and long press, as a state machine with no I/O.
 *
 * KeyService polls the key and feeds the level here with the time
 * (milliseconds, monotonic), so the timing is host-tested without waiting.
 *
 *   up --press--> Down --held LONG_PRESS_MS--> LongPress --release--> up
 *                   \--release before that---> Click
 *
 * A press gives Down, then either Click (on release) or LongPress (while
 * still held, nothing on release).
 */
class KeyGesture {
public:
    enum class Event : uint8_t { None, Down, LongPress, Click };

    static constexpr uint32_t LONG_PRESS_MS = 500;

    Event update(bool pressed, uint32_t now_ms);

    bool down() const { return m_down; }

private:
    bool m_down = false;
    bool m_long = false;       // LongPress given for this press
    uint32_t m_down_ms = 0;
};

} // namespace Input
