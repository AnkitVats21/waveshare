#pragma once

/**
 * @brief High-level assistant visual states for LED state orchestration.
 */
enum class AssistantVisualState {
    Idle,
    Listening,
    Connecting,
    Speaking,
    Thinking,
    Offline,
    Recovering,
    RateLimited,
    Error
};
