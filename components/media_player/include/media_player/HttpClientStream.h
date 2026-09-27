#pragma once
#include "esp_http_client.h"
#include <string>

class HttpClientStream {
public:
    HttpClientStream();
    ~HttpClientStream();

    // Initializes connection settings
    bool open(const std::string& url, uint32_t startByteOffset = 0);
    
    // Reads up to 'size' bytes from the active network socket into 'buffer'
    // Returns actual bytes read, 0 on completion, or -1 on network failure
    int read(uint8_t* buffer, size_t size);
    
    // Safely disconnects and cleans up memory handles
    void close();

    bool isConnected() const { return _clientHandle != nullptr && _is_connected; }
    int getStatusCode() const;

    // Of the last open(), also after it failed: the final HTTP status (0 when
    // no response arrived), the body length (-1 unknown), and the URL a
    // redirect led to (empty when there was none).
    int lastStatus() const { return _lastStatus; }
    int64_t contentLength() const { return _contentLength; }
    const std::string& finalUrl() const { return _finalUrl; }

private:
    esp_http_client_handle_t _clientHandle = nullptr;
    bool _is_connected = false;
    int _lastStatus = 0;
    int64_t _contentLength = -1;
    std::string _finalUrl;
    std::string _location;   // Location header of the response being read

    static esp_err_t _httpEventThunk(esp_http_client_event_t *evt);
};
