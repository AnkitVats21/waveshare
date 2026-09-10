#pragma once

#include <cstdint>
#include <cstddef>

namespace btplayer {

// Binary control framing: 0xAA 0x55 | type(1) | len(1) | payload[len] | crc8(1)
constexpr uint8_t UART_SYNC_1 = 0xAA;
constexpr uint8_t UART_SYNC_2 = 0x55;
constexpr size_t  UART_MAX_PAYLOAD = 64;

enum class MsgType : uint8_t {
    // Host -> Companion
    PLAY          = 0x01,
    PAUSE         = 0x02,
    STOP          = 0x03,
    SET_VOLUME    = 0x04,  // payload: u8 0-100
    BT_CONNECT    = 0x05,  // payload: optional UTF-8 speaker name
    BT_DISCONNECT = 0x06,
    PING          = 0x07,

    // Companion -> Host
    READY         = 0x81,  // payload: u16 fw version (big-endian)
    BT_STATUS     = 0x82,  // payload: u8 connected + UTF-8 name
    STATUS        = 0x83,  // payload: see StatusPayload
    PONG          = 0x84,
    LOG           = 0x85,  // payload: UTF-8 text
};

#pragma pack(push, 1)
struct StatusPayload {
    uint8_t  state;        // Companion state: 0=IDLE, 1=CONNECTING, 2=CONNECTED, 3=PLAYING, 4=ERROR
    uint8_t  playing;      // 1 while unmuted and PLAYING
    uint8_t  volume;       // 0-100
    uint8_t  pcm_buf_pct;  // 0-100
    uint32_t free_heap;    // big-endian
    uint16_t underruns;    // big-endian, cumulative
    uint16_t overruns;     // big-endian, cumulative
};
#pragma pack(pop)

inline uint8_t crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07)
                               : static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}

inline uint16_t get_be16(const uint8_t* p) {
    return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
}

inline uint32_t get_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
           static_cast<uint32_t>(p[3]);
}

} // namespace btplayer
