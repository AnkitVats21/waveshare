#pragma once
#include "esp_http_client.h"
#include <string>

class HttpClientStream {
public:
    HttpClientStream();
    ~HttpClientStream();

    // Initializes connection settings
    bool open(const std::string& url);
    
    // Reads up to 'size' bytes from the active network socket into 'buffer'
    // Returns actual bytes read, 0 on completion, or -1 on network failure
    int read(uint8_t* buffer, size_t size);
    
    // Safely disconnects and cleans up memory handles
    void close();

    bool isConnected() const { return _clientHandle != nullptr && _is_connected; }

private:
    esp_http_client_handle_t _clientHandle = nullptr;
    bool _is_connected = false;

    static esp_err_t _httpEventThunk(esp_http_client_event_t *evt);
};
