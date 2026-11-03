#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Services {

// Channel 1 Binary Framing Constants (nexus-orbit PROTOCOL.md section 3)
constexpr uint8_t CHANNEL_MIC = 1;
constexpr uint8_t FLAG_PCM    = 0x00;
constexpr uint8_t FLAG_OPUS   = 0x02;

// Mic frames: 2-byte header + up to 4096 bytes payload
constexpr size_t MIC_FRAME_MAX = 2 + 4096;

// 16 kHz mono 16-bit PCM: 20 ms = 320 samples = 640 bytes
constexpr size_t SAMPLES_PER_FRAME = 320;
constexpr size_t BYTES_PER_FRAME   = SAMPLES_PER_FRAME * sizeof(int16_t);

/**
 * @brief Accumulates arbitrary-sized PCM chunks into 320-sample (20 ms @ 16 kHz) frames.
 *
 * Preserves remainder samples across calls. Supports arbitrary chunk sizes (down to 1 byte),
 * performs zero-copy frame dispatch when input is aligned and sufficiently large, and
 * allows flushing remaining partial frames for PCM fallback.
 */
class PcmAccumulator {
public:
    static constexpr size_t FRAME_SAMPLES = SAMPLES_PER_FRAME;
    static constexpr size_t FRAME_BYTES   = BYTES_PER_FRAME;

    PcmAccumulator() { reset(); }

    void reset() {
        m_remainder_bytes = 0;
    }

    /**
     * @brief Feed raw PCM bytes into the accumulator.
     *
     * Invokes on_frame(const int16_t* frame_320) for each complete 320-sample frame.
     * Retains any leftover remainder (< 640 bytes) for subsequent calls.
     */
    template <typename Callback>
    void feed(const uint8_t* data, size_t len, Callback&& on_frame) {
        if (!data || len == 0) return;

        // 1. If we have remainder from a previous call, try to complete a 640-byte frame
        if (m_remainder_bytes > 0) {
            size_t needed = FRAME_BYTES - m_remainder_bytes;
            if (len >= needed) {
                std::memcpy(m_remainder + m_remainder_bytes, data, needed);
                on_frame(reinterpret_cast<const int16_t*>(m_remainder));
                m_remainder_bytes = 0;
                data += needed;
                len -= needed;
            } else {
                std::memcpy(m_remainder + m_remainder_bytes, data, len);
                m_remainder_bytes += len;
                return;
            }
        }

        // 2. Process complete 640-byte frames directly from input buffer
        while (len >= FRAME_BYTES) {
            if ((reinterpret_cast<uintptr_t>(data) % alignof(int16_t)) == 0) {
                // Aligned pointer: zero-copy dispatch
                on_frame(reinterpret_cast<const int16_t*>(data));
            } else {
                // Unaligned pointer: copy to aligned buffer before calling
                std::memcpy(m_remainder, data, FRAME_BYTES);
                on_frame(reinterpret_cast<const int16_t*>(m_remainder));
            }
            data += FRAME_BYTES;
            len -= FRAME_BYTES;
        }

        // 3. Store leftover bytes (< 640) in remainder buffer
        if (len > 0) {
            std::memcpy(m_remainder, data, len);
            m_remainder_bytes = len;
        }
    }

    /**
     * @brief Feed 16-bit PCM samples into the accumulator.
     */
    template <typename Callback>
    void feedSamples(const int16_t* samples, size_t count, Callback&& on_frame) {
        if (!samples || count == 0) return;
        feed(reinterpret_cast<const uint8_t*>(samples), count * sizeof(int16_t), std::forward<Callback>(on_frame));
    }

    size_t remainderBytes() const { return m_remainder_bytes; }
    size_t remainderSamples() const { return m_remainder_bytes / sizeof(int16_t); }
    const uint8_t* remainderData() const { return m_remainder; }

    /**
     * @brief Flushes remainder bytes into out_buf (must have at least FRAME_BYTES capacity).
     * @return Number of bytes flushed.
     */
    size_t flushBytes(uint8_t* out_buf);

    /**
     * @brief Flushes remainder samples into out_samples (must have at least FRAME_SAMPLES capacity).
     * Discards any dangling odd byte if present.
     * @return Number of samples flushed.
     */
    size_t flushSamples(int16_t* out_samples);

private:
    alignas(int16_t) uint8_t m_remainder[FRAME_BYTES];
    size_t m_remainder_bytes = 0;
};

/**
 * @brief Packs one or more Opus packets into a channel-1 binary frame.
 *
 * Frame structure:
 *   byte 0: CHANNEL_MIC (1)
 *   byte 1: FLAG_OPUS (0x02)
 *   payload: one or more [uint16_le len][opus packet bytes]...
 */
class OpusFrameBuilder {
public:
    OpusFrameBuilder(uint8_t* buffer, size_t capacity);

    void reset();

    /**
     * @brief Appends an Opus packet with length prefix [uint16_le len][packet].
     * @return true if added, false if it would exceed capacity.
     */
    bool addPacket(const uint8_t* packet, size_t len);

    size_t packetCount() const { return m_packet_count; }
    size_t size() const { return m_size; }
    size_t capacity() const { return m_capacity; }
    const uint8_t* data() const { return m_buffer; }
    uint8_t* data() { return m_buffer; }
    bool empty() const { return m_packet_count == 0; }

private:
    uint8_t* m_buffer;
    size_t m_capacity;
    size_t m_size = 0;
    size_t m_packet_count = 0;
};

struct OpusPacketView {
    const uint8_t* data;
    size_t len;
};

/**
 * @brief Parses a channel-1 Opus frame into individual packet views.
 * @return true if valid and well-formed, false if corrupt or truncated.
 */
bool parseOpusFrame(const uint8_t* frame, size_t len, std::vector<OpusPacketView>& out_packets);

/**
 * @brief Parses a channel-1 PCM frame.
 * @return true if valid channel 1 PCM frame, false otherwise.
 */
bool parsePcmFrame(const uint8_t* frame, size_t len, const uint8_t*& out_pcm, size_t& out_pcm_len);

/**
 * @brief Builds a channel-1 raw PCM frame: [CHANNEL_MIC, FLAG_PCM, pcm_bytes...].
 * @return Total frame length (2 + pcm_len), or 0 if buffer capacity is insufficient.
 */
size_t buildPcmFrame(uint8_t* buffer, size_t capacity, const uint8_t* pcm, size_t pcm_len);

} // namespace Services
