#pragma once

#include <cstddef>
#include <cstdint>

struct OpusEncoder;

namespace Services {

/**
 * @brief Raw Opus packet encoder for mic uplink (docs/PROTOCOL.md section 3.1 & 4).
 *
 * 16 kHz mono, OPUS_APPLICATION_RESTRICTED_LOWDELAY (CELT-only mode; SILK at 16 kHz
 * exceeds 100% of an ESP32-S3 core due to software floating-point emulation).
 *
 * State and packet buffers are allocated entirely in PSRAM (MALLOC_CAP_SPIRAM).
 * Thread-safe pseudostack is configured in PSRAM via CONFIG_OPUS_THREADSAFE_PSEUDOSTACK.
 */
class UplinkEncoder {
public:
    static constexpr size_t FRAME_SAMPLES = 320; // 20 ms @ 16 kHz mono
    static constexpr size_t MAX_PACKET_BYTES = 1275;

    UplinkEncoder();
    ~UplinkEncoder();

    UplinkEncoder(const UplinkEncoder&) = delete;
    UplinkEncoder& operator=(const UplinkEncoder&) = delete;

    /**
     * @brief Creates and initializes the Opus encoder state in PSRAM.
     * @return true on success, false on allocation or initialization failure.
     */
    bool init(int kbps = 24, int complexity = 3);

    /**
     * @brief Resets encoder state (OPUS_RESET_STATE) and updates bitrate/complexity.
     * @return true on success, false if encoder is not initialized.
     */
    bool reset(int kbps = 24, int complexity = 3);

    /**
     * @brief Encodes one 20 ms frame (320 samples).
     * @param pcm_320 Pointer to 320 16-bit PCM samples.
     * @param out_packet Buffer to receive the encoded Opus packet (at least MAX_PACKET_BYTES).
     * @param max_packet_bytes Capacity of out_packet.
     * @return Length of encoded packet in bytes, or negative error code from opus_encode.
     */
    int encode(const int16_t* pcm_320, uint8_t* out_packet, size_t max_packet_bytes);

    bool isInitialized() const { return m_enc != nullptr; }

private:
    OpusEncoder* m_enc = nullptr;
    int m_kbps = 0;
    int m_complexity = -1;
};

} // namespace Services
