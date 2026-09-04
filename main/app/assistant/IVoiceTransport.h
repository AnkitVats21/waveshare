#pragma once

#include <cstdint>
#include <cstddef>

/**
 * @brief Abstract interface for Voice Assistant Network Transports.
 *
 * Decouples high-level Assistant business logic and audio pumping from the
 * physical wire protocol.
 *
 * Supported implementations:
 *   1. Direct Mode: GeminiProtocol (Direct WebSocket to Google Cloud)
 *   2. Relayed Mode: RelayVoiceClient (WebSocket to local Raspberry Pi Hub)
 */
class IVoiceTransport {
public:
    virtual ~IVoiceTransport() = default;

    /** @brief Check if transport connection to backend is alive. */
    virtual bool isConnected() = 0;

    /** @brief Open connection to the backend service. */
    virtual void connect() = 0;

    /** @brief Close connection cleanly. */
    virtual void closeConnection() = 0;

    /** @brief Force immediate reconnect. */
    virtual void forceReconnect() = 0;

    /** @brief Transmit microphone audio PCM (uplink). */
    virtual void transmitAudioUplink(const char* base64_pcm) = 0;

    /** @brief Transmit function/tool execution response back to AI. */
    virtual void transmitToolResponse(const char* call_id, const char* json_result) = 0;

    /** @brief Transmit direct user text prompt. */
    virtual void sendTextDirect(const char* text) = 0;
};
