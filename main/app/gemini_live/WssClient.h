#pragma once

#include "esp_websocket_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

/**
 * @brief RAII wrapper for esp_websocket_client_handle_t.
 * Manages the lifecycle of the WebSocket client handle, ensuring proper 
 * initialization, resource cleanup, and providing a cleaner interface 
 * for common operations.
 */
class WssClient {
public:
    WssClient() : m_handle(nullptr) {}
    
    /**
     * @brief Destructor ensures the client is destroyed, freeing allocated resources.
     */
    ~WssClient() {
        destroy();
    }

    // Non-copyable to prevent multiple management of the same handle
    WssClient(const WssClient&) = delete;
    WssClient& operator=(const WssClient&) = delete;

    // Move-constructible
    WssClient(WssClient&& other) noexcept : m_handle(other.m_handle) {
        other.m_handle = nullptr;
    }
    
    // Move-assignable
    WssClient& operator=(WssClient&& other) noexcept {
        if (this != &other) {
            destroy();
            m_handle = other.m_handle;
            other.m_handle = nullptr;
        }
        return *this;
    }

    /**
     * @brief Initializes the WebSocket client with the given configuration.
     * @param config The configuration for the WebSocket client.
     * @return true if initialization was successful, false otherwise.
     */
    bool init(const esp_websocket_client_config_t& config) {
        destroy();
        m_handle = esp_websocket_client_init(&config);
        return m_handle != nullptr;
    }

    /**
     * @brief Destroys the WebSocket client handle if it exists.
     */
    void destroy() {
        if (m_handle) {
            esp_websocket_client_destroy(m_handle);
            m_handle = nullptr;
        }
    }

    /**
     * @brief Starts the WebSocket client connection.
     */
    esp_err_t start() {
        if (!m_handle) return ESP_ERR_INVALID_STATE;
        return esp_websocket_client_start(m_handle);
    }

    /**
     * @brief Stops the WebSocket client connection.
     */
    esp_err_t stop() {
        if (!m_handle) return ESP_ERR_INVALID_STATE;
        return esp_websocket_client_stop(m_handle);
    }

    /**
     * @brief Closes the WebSocket connection gracefully.
     * @param timeout Timeout for the close operation.
     */
    esp_err_t close(TickType_t timeout = pdMS_TO_TICKS(1000)) {
        if (!m_handle) return ESP_ERR_INVALID_STATE;
        return esp_websocket_client_close(m_handle, timeout);
    }

    /**
     * @brief Sends text data over the WebSocket connection.
     * @param data The text data to send.
     * @param len The length of the data.
     * @param timeout Timeout for the send operation.
     * @return Number of bytes sent, or -1 on error.
     */
    int sendText(const char* data, int len, TickType_t timeout = pdMS_TO_TICKS(1000)) {
        if (!m_handle) return -1;
        return esp_websocket_client_send_text(m_handle, data, len, timeout);
    }

    /**
     * @brief Checks if the WebSocket client is currently connected.
     */
    bool isConnected() const {
        if (!m_handle) return false;
        return esp_websocket_client_is_connected(m_handle);
    }

    /**
     * @brief Registers an event handler for WebSocket events.
     * @param event_handler The handler function.
     * @param event_handler_arg Argument passed to the handler.
     * @param event_id The event ID to register for (defaults to all events).
     */
    esp_err_t registerEvents(esp_event_handler_t event_handler, void* event_handler_arg, esp_websocket_event_id_t event_id = WEBSOCKET_EVENT_ANY) {
        if (!m_handle) return ESP_ERR_INVALID_STATE;
        return esp_websocket_register_events(m_handle, event_id, event_handler, event_handler_arg);
    }

    /**
     * @brief Returns the underlying handle.
     */
    esp_websocket_client_handle_t getHandle() const { return m_handle; }
    
    /**
     * @brief Explicit boolean conversion to check if the client is initialized.
     */
    explicit operator bool() const { return m_handle != nullptr; }

private:
    esp_websocket_client_handle_t m_handle;
};
