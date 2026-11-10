#include "app/input/KeyGesture.h"

namespace Input {

KeyGesture::Event KeyGesture::update(bool pressed, uint32_t now_ms) {
    if (pressed && !m_down) {
        m_down = true;
        m_long = false;
        m_down_ms = now_ms;
        return Event::Down;
    }
    if (pressed) {
        // Unsigned difference: correct across the millisecond counter's wrap.
        if (!m_long && now_ms - m_down_ms >= LONG_PRESS_MS) {
            m_long = true;
            return Event::LongPress;
        }
        return Event::None;
    }
    if (!m_down) return Event::None;
    m_down = false;
    return m_long ? Event::None : Event::Click;
}

} // namespace Input
