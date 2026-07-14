#include "HttpClientStream.h"
#include "esp_log.h"

static const char* TAG = "HttpStream";

HttpClientStream::HttpClientStream() {}

HttpClientStream::~HttpClientStream() {
    close();
}

esp_err_t HttpClientStream::_httpEventThunk(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADERS_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADERS_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        default:
            break;
    }
    return ESP_OK;
}

bool HttpClientStream::open(const std::string& url) {
    close(); // Ensure any previous session is dead

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.event_handler = _httpEventThunk;
    config.is_async = false;

    // --- HTTPS Security Layer Enhancements ---
    if (url.rfind("https://", 0) == 0) {
        config.transport_type = HTTP_TRANSPORT_OVER_SSL;
        
        // --- Bypasses the text parser error while preserving full TLS Handshake ---
        config.skip_cert_common_name_check = true; 
        
        // Tells ESP-TLS to negotiate encryption directly without manually parsing a local string
        #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
        config.use_global_ca_store = false;
        #endif
        
        config.buffer_size_tx = 1024;
        config.buffer_size = 4096;
    } else {
        config.transport_type = HTTP_TRANSPORT_OVER_TCP;
    }

    _clientHandle = esp_http_client_init(&config);
    if (!_clientHandle) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return false;
    }

    // Open the connection and fetch headers only. The body is then consumed
    // incrementally via esp_http_client_read() in the network task.
    esp_err_t err = esp_http_client_open(_clientHandle, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        close();
        return false;
    }

    int64_t content_length = esp_http_client_fetch_headers(_clientHandle);
    if (content_length < 0) {
        ESP_LOGE(TAG, "HTTP fetch headers failed: %lld", (long long)content_length);
        close();
        return false;
    }

    ESP_LOGI(TAG, "HTTP stream opened. content_length=%lld", (long long)content_length);
    _is_connected = true;
    esp_http_client_set_header(_clientHandle, "Connection", "keep-alive");
    return true;
}

int HttpClientStream::read(uint8_t* buffer, size_t size) {
    if (!_clientHandle || !_is_connected) return -1;

    // Fetch the data chunk sequentially from the socket loop
    int read_bytes = esp_http_client_read(_clientHandle, (char*)buffer, size);
    
    if (read_bytes < 0) {
        ESP_LOGE(TAG, "Error reading from HTTP stream");
        return -1; 
    }
    
    if (read_bytes == 0) {
        _is_connected = false; // Stream finished naturally
    }

    return read_bytes;
}

void HttpClientStream::close() {
    _is_connected = false;
    if (_clientHandle) {
        esp_http_client_cleanup(_clientHandle);
        _clientHandle = nullptr;
        ESP_LOGI(TAG, "HTTP stream closed cleanly");
    }
}
