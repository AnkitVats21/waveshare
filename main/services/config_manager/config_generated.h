#pragma once
#include <string.h>
#include <stdbool.h>

namespace Services {

struct WifiConfig {
    char ssid[64];
    char password[64];
    bool static_ip;
    char ip[16];
    char mask[16];
    char gateway[16];
    char dns[16];
};

struct MqttConfig {
    char broker_uri[128];
    char client_id[64];
    char username[64];
    char password[64];
};

struct SystemConfig {
    WifiConfig wifi;
    MqttConfig mqtt;
};

void InitializeDefaultConfig(SystemConfig* config);
bool UpdateConfigValue(SystemConfig* config, const char* section, const char* key, const char* value);

} // namespace Services
