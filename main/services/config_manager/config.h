#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include "config_generated.h"
#include <mutex>
#include <string>

namespace Services {

class ConfigManager {
public:
    static ConfigManager& getInstance();

    // Get a copy of the current configuration (thread-safe)
    SystemConfig getConfig();

    // Load configuration from SD card (settings.txt)
    bool loadFromSDCard(const char* path = "/sdcard/settings.txt");

    // Update a specific value at runtime
    bool updateValue(const char* section, const char* key, const char* value);

private:
    ConfigManager();
    ~ConfigManager() = default;
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    SystemConfig _config;
    std::mutex _mutex;
};

// Global helper functions
SystemConfig GetConfig();
bool LoadConfigFromSD(const char* path = "/sdcard/settings.txt");
bool UpdateConfig(const char* section, const char* key, const char* value);

} // namespace Services

#endif // CONFIG_MANAGER_H