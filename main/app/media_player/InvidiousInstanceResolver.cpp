#include "InvidiousInstanceResolver.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include <ArduinoJson.h>

static const char* TAG = "InvidiousResolver";

namespace {

static const std::vector<std::string> DEFAULT_FALLBACK_INSTANCES = {
    "stream.ankitm.xyz",
    "192.168.1.21:8088",
    "invidious.flokinet.to",
    "invidious.f5.si",
    "invidious.tiekoetter.com"
};

esp_err_t httpEventCollector(esp_http_client_event_t* evt) {
    if (!evt) return ESP_OK;
    auto* response = static_cast<std::string*>(evt->user_data);

    if (evt->event_id == HTTP_EVENT_ON_DATA && response && evt->data && evt->data_len > 0) {
        response->append(static_cast<const char*>(evt->data), evt->data_len);
    }
    return ESP_OK;
}

} // namespace

InvidiousInstanceResolver& InvidiousInstanceResolver::getInstance() {
    static InvidiousInstanceResolver instance;
    return instance;
}

InvidiousInstanceResolver::InvidiousInstanceResolver() {
    _customHost = "stream.ankitm.xyz";
    _instances = DEFAULT_FALLBACK_INSTANCES;
}

void InvidiousInstanceResolver::setCustomInstance(const std::string& host) {
    _customHost = host;
    ESP_LOGI(TAG, "Custom Invidious instance host configured: %s", _customHost.c_str());
}

std::string InvidiousInstanceResolver::getActiveInstance() {
    if (!_customHost.empty()) {
        return _customHost;
    }
    if (!_initialized || _instances.empty()) {
        refreshInstances();
    }
    if (_instances.empty()) {
        return "invidious.flokinet.to";
    }
    return _instances[_currentIndex % _instances.size()];
}

void InvidiousInstanceResolver::markInstanceFailed() {
    if (_instances.empty()) return;
    ESP_LOGW(TAG, "Instance %s failed; rotating to next", _instances[_currentIndex % _instances.size()].c_str());
    _currentIndex = (_currentIndex + 1) % _instances.size();
}

bool InvidiousInstanceResolver::testInstance(const std::string& host) {
    std::string url = "https://" + host + "/api/v1/stats";

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = 4000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.skip_cert_common_name_check = true;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;

    esp_http_client_set_header(client, "User-Agent", "Mozilla/5.0 (ESP32-S3 Waveshare)");
    esp_err_t err = esp_http_client_perform(client);
    bool ok = false;
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ok = (status == 200);
        ESP_LOGD(TAG, "Instance %s test status: %d", host.c_str(), status);
    } else {
        ESP_LOGD(TAG, "Instance %s test failed: %s", host.c_str(), esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return ok;
}

bool InvidiousInstanceResolver::queryPublicInstanceList() {
    const char* url = "https://api.invidious.io/instances.json?sort_by=health,type";
    std::string response;

    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = httpEventCollector;
    config.user_data = &response;
    config.timeout_ms = 5000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.skip_cert_common_name_check = true;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;

    esp_http_client_set_header(client, "User-Agent", "Mozilla/5.0 (ESP32-S3 Waveshare)");
    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || response.empty()) {
        ESP_LOGW(TAG, "Failed to fetch public instances (err=%s, status=%d)", esp_err_to_name(err), status);
        return false;
    }

    JsonDocument filter;
    filter[0][0] = true;
    filter[0][1]["type"] = true;
    filter[0][1]["monitor"]["down"] = true;

    JsonDocument doc;
    DeserializationError jsonErr = deserializeJson(doc, response, DeserializationOption::Filter(filter));
    if (jsonErr) {
        ESP_LOGE(TAG, "Failed to parse instances JSON: %s", jsonErr.c_str());
        return false;
    }

    std::vector<std::string> discovered;
    for (JsonVariant item : doc.as<JsonArray>()) {
        const char* domain = item[0];
        const char* type = item[1]["type"];
        bool down = item[1]["monitor"]["down"] | false;

        if (domain && type && strcmp(type, "https") == 0 && !down) {
            discovered.push_back(domain);
        }
    }

    if (!discovered.empty()) {
        ESP_LOGI(TAG, "Discovered %zu online instances from api.invidious.io", discovered.size());
        _instances = discovered;
        return true;
    }

    return false;
}

bool InvidiousInstanceResolver::refreshInstances() {
    ESP_LOGI(TAG, "Refreshing Invidious instances...");
    if (!queryPublicInstanceList()) {
        ESP_LOGI(TAG, "Using fallback instance list (%zu instances)", DEFAULT_FALLBACK_INSTANCES.size());
        _instances = DEFAULT_FALLBACK_INSTANCES;
    }

    // Probe the top candidate to ensure immediate usability
    for (size_t i = 0; i < _instances.size() && i < 3; ++i) {
        if (testInstance(_instances[i])) {
            _currentIndex = i;
            _initialized = true;
            ESP_LOGI(TAG, "Selected active Invidious instance: %s", _instances[i].c_str());
            return true;
        }
    }

    _currentIndex = 0;
    _initialized = true;
    ESP_LOGW(TAG, "Defaulting to initial instance: %s", _instances.empty() ? "none" : _instances[0].c_str());
    return !_instances.empty();
}
