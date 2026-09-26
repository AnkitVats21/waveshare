#pragma once

/**
 * @brief Abstract interface for platform/board specific device command execution.
 * Decouples Gemini tool dispatching from board services (the alarm scheduler).
 * File tools use sd_storage directly.
 */
class IDeviceCommandDelegate {
public:
    virtual ~IDeviceCommandDelegate() = default;

    // Alarm scheduler operations
    virtual bool setAlarm(int hour, int minute, const char* tone_file, bool enabled, int& out_alarm_id) = 0;
    virtual bool stopActiveAlarm() = 0;
};
