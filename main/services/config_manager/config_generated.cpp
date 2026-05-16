#include "config_generated.h"
#include <string.h>
#include <stdlib.h>

namespace Services {

void InitializeDefaultConfig(SystemConfig* config) {
    memset(config, 0, sizeof(SystemConfig));
    strncpy(config->wifi.ssid, "", sizeof(config->wifi.ssid) - 1);
    strncpy(config->wifi.password, "", sizeof(config->wifi.password) - 1);
    config->wifi.static_ip = false;
    strncpy(config->wifi.ip, "0.0.0.0", sizeof(config->wifi.ip) - 1);
    strncpy(config->wifi.mask, "255.255.255.0", sizeof(config->wifi.mask) - 1);
    strncpy(config->wifi.gateway, "0.0.0.0", sizeof(config->wifi.gateway) - 1);
    strncpy(config->wifi.dns, "8.8.8.8", sizeof(config->wifi.dns) - 1);
    strncpy(config->mqtt.broker_uri, "mqtt://broker.emqx.io", sizeof(config->mqtt.broker_uri) - 1);
    strncpy(config->mqtt.client_id, "esp32_device", sizeof(config->mqtt.client_id) - 1);
    strncpy(config->mqtt.username, "", sizeof(config->mqtt.username) - 1);
    strncpy(config->mqtt.password, "", sizeof(config->mqtt.password) - 1);
}

bool UpdateConfigValue(SystemConfig* config, const char* section, const char* key, const char* value) {
    if (strcmp(section, "wifi") == 0) {
        if (strcmp(key, "ssid") == 0) {
            strncpy(config->wifi.ssid, value, sizeof(config->wifi.ssid) - 1);
            config->wifi.ssid[sizeof(config->wifi.ssid) - 1] = '\0';
            return true;
        } else if (strcmp(key, "password") == 0) {
            strncpy(config->wifi.password, value, sizeof(config->wifi.password) - 1);
            config->wifi.password[sizeof(config->wifi.password) - 1] = '\0';
            return true;
        } else if (strcmp(key, "static_ip") == 0) {
            config->wifi.static_ip = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            return true;
        } else if (strcmp(key, "ip") == 0) {
            strncpy(config->wifi.ip, value, sizeof(config->wifi.ip) - 1);
            config->wifi.ip[sizeof(config->wifi.ip) - 1] = '\0';
            return true;
        } else if (strcmp(key, "mask") == 0) {
            strncpy(config->wifi.mask, value, sizeof(config->wifi.mask) - 1);
            config->wifi.mask[sizeof(config->wifi.mask) - 1] = '\0';
            return true;
        } else if (strcmp(key, "gateway") == 0) {
            strncpy(config->wifi.gateway, value, sizeof(config->wifi.gateway) - 1);
            config->wifi.gateway[sizeof(config->wifi.gateway) - 1] = '\0';
            return true;
        } else if (strcmp(key, "dns") == 0) {
            strncpy(config->wifi.dns, value, sizeof(config->wifi.dns) - 1);
            config->wifi.dns[sizeof(config->wifi.dns) - 1] = '\0';
            return true;
        }
    } else if (strcmp(section, "mqtt") == 0) {
        if (strcmp(key, "broker_uri") == 0) {
            strncpy(config->mqtt.broker_uri, value, sizeof(config->mqtt.broker_uri) - 1);
            config->mqtt.broker_uri[sizeof(config->mqtt.broker_uri) - 1] = '\0';
            return true;
        } else if (strcmp(key, "client_id") == 0) {
            strncpy(config->mqtt.client_id, value, sizeof(config->mqtt.client_id) - 1);
            config->mqtt.client_id[sizeof(config->mqtt.client_id) - 1] = '\0';
            return true;
        } else if (strcmp(key, "username") == 0) {
            strncpy(config->mqtt.username, value, sizeof(config->mqtt.username) - 1);
            config->mqtt.username[sizeof(config->mqtt.username) - 1] = '\0';
            return true;
        } else if (strcmp(key, "password") == 0) {
            strncpy(config->mqtt.password, value, sizeof(config->mqtt.password) - 1);
            config->mqtt.password[sizeof(config->mqtt.password) - 1] = '\0';
            return true;
        }
    }
    return false;
}

} // namespace Services
