#include "HttpClientStream.h"
#include "media_player/TlsConfig.h"
#include "esp_log.h"
#include <cstring>

static const char* TAG = "HttpStream";
static constexpr int kMaxRedirects = 5;

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
            // Kept for the redirect: esp_http_client_get_url() drops the query
            // string, which holds a googlevideo URL's signature.
            if (evt->user_data && strcasecmp(evt->header_key, "Location") == 0) {
                static_cast<HttpClientStream*>(evt->user_data)->_location = evt->header_value;
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        default:
            break;
    }
    return ESP_OK;
}

bool HttpClientStream::open(const std::string& url, uint32_t startByteOffset) {
    close(); // Ensure any previous session is dead
    _finalUrl.clear();
    _lastStatus = 0;
    _contentLength = -1;

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.event_handler = _httpEventThunk;
    config.user_data = this;
    config.is_async = false;
    config.timeout_ms = 10000;

    if (url.rfind("https://", 0) == 0) {
        config.transport_type = HTTP_TRANSPORT_OVER_SSL;
        Tls::secure(config);
        config.buffer_size_tx = 4096;
        config.buffer_size = 8192;
    } else {
        config.transport_type = HTTP_TRANSPORT_OVER_TCP;
        config.buffer_size_tx = 4096;
        config.buffer_size = 8192;
    }

    _clientHandle = esp_http_client_init(&config);
    if (!_clientHandle) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return false;
    }

    esp_http_client_set_header(_clientHandle, "User-Agent", "Mozilla/5.0 (ESP32-S3 Waveshare)");

    // YouTube's googlevideo CDN throttles plain GETs on `videoplayback` URLs to
    // ~32 KB/s. Sending an open-ended Range header makes it serve at full link speed.
    // When seeking, startByteOffset positions the download at the desired keyframe.
    char rangeHeader[48];
    snprintf(rangeHeader, sizeof(rangeHeader), "bytes=%u-", (unsigned int)startByteOffset);
    esp_http_client_set_header(_clientHandle, "Range", rangeHeader);

    // Open the connection and fetch headers only. The body is then consumed
    // incrementally via esp_http_client_read() in the network task.
    // esp_http_client follows redirects only inside esp_http_client_perform(),
    // so they are followed here. googlevideo answers with a 302 to a nearby
    // cache when the URL was signed for another address (a resolver behind a
    // VPN such as WARP); headers set above, including Range, carry over.
    int64_t content_length = -1;
    int status = 0;
    for (int redirects = 0;; ++redirects) {
        esp_err_t err = esp_http_client_open(_clientHandle, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
            close();
            return false;
        }

        content_length = esp_http_client_fetch_headers(_clientHandle);
        if (content_length < 0) {
            ESP_LOGE(TAG, "HTTP fetch headers failed: %lld", (long long)content_length);
            close();
            return false;
        }

        status = esp_http_client_get_status_code(_clientHandle);
        _lastStatus = status;
        const bool isRedirect = status == HttpStatus_MovedPermanently || status == HttpStatus_Found ||
                                status == HttpStatus_SeeOther || status == HttpStatus_TemporaryRedirect ||
                                status == HttpStatus_PermanentRedirect;
        if (!isRedirect) break;
        if (redirects >= kMaxRedirects) {
            ESP_LOGE(TAG, "Too many redirects (%d)", redirects);
            close();
            return false;
        }

        esp_http_client_flush_response(_clientHandle, nullptr);
        // An absolute Location is remembered, so the next open (a seek or a
        // reconnect) can go straight to it; a relative one is only followed.
        if (_location.rfind("http", 0) == 0) {
            _finalUrl = _location;
        } else {
            _finalUrl.clear();
        }
        _location.clear();
        if (esp_http_client_set_redirection(_clientHandle) != ESP_OK) {
            ESP_LOGE(TAG, "HTTP %d without a usable Location", status);
            close();
            return false;
        }
        esp_http_client_close(_clientHandle);

        // Log the host only: the query string is a signed, IP-bound URL.
        char next[128] = {};
        esp_http_client_get_url(_clientHandle, next, sizeof(next));
        if (char* q = strchr(next, '?')) *q = '\0';
        ESP_LOGI(TAG, "HTTP %d, following redirect to %s", status, next);
    }

    ESP_LOGI(TAG, "HTTP stream opened (status=%d, offset=%u, content_length=%lld)",
             status, (unsigned int)startByteOffset, (long long)content_length);
    if (status != HttpStatus_Ok && status != HttpStatus_PartialContent) {
        // An error page (e.g. 403 for an expired link) is not audio; failing
        // here reports a playback error instead of a track that "finished".
        ESP_LOGE(TAG, "HTTP stream failed with status %d", status);
        close();
        return false;
    }
    if (startByteOffset > 0 && status != HttpStatus_PartialContent) {
        // The whole file from byte 0: continuing a seek or a reconnect with it
        // would feed the decoder the wrong bytes.
        ESP_LOGE(TAG, "Server ignored the range (status %d at offset %u)", status, (unsigned)startByteOffset);
        close();
        return false;
    }

    _contentLength = content_length;
    _is_connected = true;
    esp_http_client_set_header(_clientHandle, "Connection", "keep-alive");
    return true;
}

int HttpClientStream::getStatusCode() const {
    return _clientHandle ? esp_http_client_get_status_code(_clientHandle) : -1;
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
