#pragma once

#include <string>

/**
 * @brief Abstract interface for platform/board specific device command execution.
 * Decouples Gemini tool dispatching from local hardware and board services
 * (storage/SD card and alarm scheduler).
 */
class IDeviceCommandDelegate {
public:
    virtual ~IDeviceCommandDelegate() = default;

    // Storage / Filesystem operations
    virtual bool writeFile(const char* path, const char* content) = 0;
    virtual std::string readFile(const char* path) = 0;
    virtual bool fileExists(const char* path) = 0;
    virtual bool appendFile(const char* path, const char* content) = 0;

    // Alarm scheduler operations
    virtual bool setAlarm(int hour, int minute, const char* tone_file, bool enabled, int& out_alarm_id) = 0;
    virtual bool stopActiveAlarm() = 0;
};
