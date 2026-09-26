#include "credentials/Credentials.h"

#include <ArduinoJson.h>

#include "esp_log.h"
#include "nvs.h"
#include "sd_storage/Fs.h"
#include "sd_storage/SdCard.h"

namespace credentials {
namespace {

const char* const TAG = "Credentials";

constexpr const char* CREDS_NS = "creds";
constexpr const char* GEMINI_KEY = "gemini_api_key";
constexpr const char* WIFI_NS = "wifi_store";

constexpr const char* WIFI_FILE = "/sdcard/wifi_config.json";
constexpr const char* GEMINI_FILE = "/sdcard/gemini_config.json";
constexpr const char* SETTINGS_FILE = "/sdcard/settings.txt";

std::string readString(const char* ns, const char* key) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return "";
    std::string value;
    size_t len = 0;
    if (nvs_get_str(h, key, nullptr, &len) == ESP_OK && len > 1) {
        value.resize(len);
        if (nvs_get_str(h, key, value.data(), &len) == ESP_OK) {
            value.resize(len - 1);  // drop the NUL
        } else {
            value.clear();
        }
    }
    nvs_close(h);
    return value;
}

bool writeStrings(const char* ns, std::initializer_list<std::pair<const char*, const std::string*>> values) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        for (const auto& [key, value] : values) {
            if ((err = nvs_set_str(h, key, value->c_str())) != ESP_OK) break;
        }
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "NVS write to '%s' failed: %s", ns, esp_err_to_name(err));
    return err == ESP_OK;
}

// "api_key" from a gemini_config.json that no longer parses (e.g. truncated).
std::string salvageApiKey(const std::string& content) {
    size_t k = content.find("\"api_key\"");
    if (k == std::string::npos) return "";
    size_t colon = content.find(':', k);
    size_t open = (colon == std::string::npos) ? colon : content.find('"', colon);
    size_t close = (open == std::string::npos) ? open : content.find('"', open + 1);
    if (close == std::string::npos) return "";
    return content.substr(open + 1, close - open - 1);
}

void importWifi() {
    if (!sd_storage::Fs::isFile(WIFI_FILE)) return;
    JsonDocument doc;
    std::string content = sd_storage::Fs::readText(WIFI_FILE);
    Wifi wifi;
    if (!deserializeJson(doc, content)) {
        wifi.ssid = doc["ssid"] | "";
        wifi.password = doc["password"] | "";
    }
    if (wifi.ssid.empty()) {
        ESP_LOGW(TAG, "%s has no usable ssid; left in place", WIFI_FILE);
        return;
    }
    Wifi stored;
    if (!saveWifi(wifi) || !loadWifi(stored) || stored.ssid != wifi.ssid || stored.password != wifi.password) {
        ESP_LOGE(TAG, "Wi-Fi credentials did not read back from NVS; %s left in place", WIFI_FILE);
        return;
    }
    sd_storage::Fs::remove(WIFI_FILE);
    ESP_LOGI(TAG, "Moved Wi-Fi credentials for SSID '%s' from the SD card to NVS", wifi.ssid.c_str());
}

void importGeminiKey() {
    std::string content = sd_storage::Fs::readText(GEMINI_FILE);
    if (content.find("\"api_key\"") == std::string::npos) return;

    JsonDocument doc;
    bool parsed = !deserializeJson(doc, content) && doc.is<JsonObject>();
    std::string key = parsed ? (doc["api_key"] | "") : salvageApiKey(content);
    if (!key.empty() && (!setGeminiApiKey(key) || geminiApiKey() != key)) {
        ESP_LOGE(TAG, "Gemini API key did not read back from NVS; %s left in place", GEMINI_FILE);
        return;
    }

    // Keep the other settings; an unreadable file has none left to keep.
    if (!parsed) doc.to<JsonObject>();
    doc.remove("api_key");
    std::string out;
    serializeJson(doc, out);
    if (!sd_storage::Fs::writeAtomic(GEMINI_FILE, out.c_str())) {
        ESP_LOGE(TAG, "Could not rewrite %s without the API key", GEMINI_FILE);
        return;
    }
    ESP_LOGI(TAG, "Moved the Gemini API key from the SD card to NVS");
}

void dropSettingsSecrets() {
    std::string content = sd_storage::Fs::readText(SETTINGS_FILE);
    std::string kept;
    size_t dropped = 0;
    for (size_t pos = 0; pos < content.size();) {
        size_t end = content.find('\n', pos);
        end = (end == std::string::npos) ? content.size() : end + 1;
        std::string_view line(content.data() + pos, end - pos);
        size_t start = line.find_first_not_of(" \t");
        if (start != std::string_view::npos && (line.substr(start, 5) == "wifi." || line.substr(start, 5) == "mqtt.")) {
            ++dropped;
        } else {
            kept.append(line);
        }
        pos = end;
    }
    if (dropped == 0) return;
    if (sd_storage::Fs::writeAtomic(SETTINGS_FILE, kept.c_str())) {
        ESP_LOGI(TAG, "Removed %zu unused credential lines from %s", dropped, SETTINGS_FILE);
    } else {
        ESP_LOGE(TAG, "Could not rewrite %s", SETTINGS_FILE);
    }
}

}  // namespace

std::string geminiApiKey() {
    return readString(CREDS_NS, GEMINI_KEY);
}

bool hasGeminiApiKey() {
    return !geminiApiKey().empty();
}

bool setGeminiApiKey(const std::string& key) {
    return !key.empty() && writeStrings(CREDS_NS, {{GEMINI_KEY, &key}});
}

bool loadWifi(Wifi& out) {
    out.ssid = readString(WIFI_NS, "ssid");
    out.password = readString(WIFI_NS, "password");
    return !out.ssid.empty();
}

bool saveWifi(const Wifi& wifi) {
    return !wifi.ssid.empty() && writeStrings(WIFI_NS, {{"ssid", &wifi.ssid}, {"password", &wifi.password}});
}

bool clearWifi() {
    nvs_handle_t h;
    if (nvs_open(WIFI_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_erase_all(h) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

void importFromSdCard() {
    if (!sd_storage::SdCard::instance().isMounted()) return;
    importWifi();
    importGeminiKey();
    dropSettingsSecrets();
}

}  // namespace credentials
