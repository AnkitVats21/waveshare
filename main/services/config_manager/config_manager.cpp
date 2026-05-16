#include "config.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char* TAG = "ConfigManager";

namespace Services {

ConfigManager& ConfigManager::getInstance() {
    static ConfigManager instance;
    return instance;
}

ConfigManager::ConfigManager() {
    std::lock_guard<std::mutex> lock(_mutex);
    InitializeDefaultConfig(&_config);
}

SystemConfig ConfigManager::getConfig() {
    std::lock_guard<std::mutex> lock(_mutex);
    return _config;
}

bool ConfigManager::updateValue(const char* section, const char* key, const char* value) {
    std::lock_guard<std::mutex> lock(_mutex);
    return UpdateConfigValue(&_config, section, key, value);
}

bool ConfigManager::loadFromSDCard(const char* path) {
    FILE* f = fopen(path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open %s for reading", path);
        return false;
    }

    char line[256];
    int line_count = 0;
    while (fgets(line, sizeof(line), f)) {
        line_count++;
        // Remove newline
        line[strcspn(line, "\r\n")] = 0;

        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\0' || line[0] == ';') continue;

        // Parse: section.key=value
        char* eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char* value = eq + 1;
        char* full_key = line;

        char* dot = strchr(full_key, '.');
        if (!dot) {
            ESP_LOGW(TAG, "Invalid line %d: %s (expected section.key=value)", line_count, full_key);
            continue;
        }

        *dot = '\0';
        char* section = full_key;
        char* key = dot + 1;

        if (updateValue(section, key, value)) {
            ESP_LOGI(TAG, "Updated [%s] %s = %s", section, key, value);
        } else {
            ESP_LOGW(TAG, "Failed to update [%s] %s (unknown field)", section, key);
        }
    }

    fclose(f);
    ESP_LOGI(TAG, "Config loaded from %s", path);
    return true;
}

// Global helpers
SystemConfig GetConfig() {
    return ConfigManager::getInstance().getConfig();
}

bool LoadConfigFromSD(const char* path) {
    return ConfigManager::getInstance().loadFromSDCard(path);
}

bool UpdateConfig(const char* section, const char* key, const char* value) {
    return ConfigManager::getInstance().updateValue(section, key, value);
}

} // namespace Services
