#include "services/hub/UplinkFraming.h"

namespace Services {

size_t PcmAccumulator::flushBytes(uint8_t* out_buf) {
    if (!out_buf || m_remainder_bytes == 0) {
        m_remainder_bytes = 0;
        return 0;
    }
    size_t flushed = m_remainder_bytes;
    std::memcpy(out_buf, m_remainder, flushed);
    m_remainder_bytes = 0;
    return flushed;
}

size_t PcmAccumulator::flushSamples(int16_t* out_samples) {
    if (!out_samples || m_remainder_bytes < sizeof(int16_t)) {
        m_remainder_bytes = 0;
        return 0;
    }
    size_t samples = m_remainder_bytes / sizeof(int16_t);
    std::memcpy(out_samples, m_remainder, samples * sizeof(int16_t));
    m_remainder_bytes = 0;
    return samples;
}

OpusFrameBuilder::OpusFrameBuilder(uint8_t* buffer, size_t capacity)
    : m_buffer(buffer), m_capacity(capacity)
{
    reset();
}

void OpusFrameBuilder::reset() {
    if (m_buffer && m_capacity >= 2) {
        m_buffer[0] = CHANNEL_MIC;
        m_buffer[1] = FLAG_OPUS;
        m_size = 2;
    } else {
        m_size = 0;
    }
    m_packet_count = 0;
}

bool OpusFrameBuilder::addPacket(const uint8_t* packet, size_t len) {
    if (!m_buffer || len > 0xFFFF) return false;
    // Each packet adds 2-byte little-endian length + payload bytes
    if (m_size + 2 + len > m_capacity) {
        return false;
    }
    m_buffer[m_size]     = static_cast<uint8_t>(len & 0xFF);
    m_buffer[m_size + 1] = static_cast<uint8_t>((len >> 8) & 0xFF);
    if (len > 0 && packet) {
        std::memcpy(m_buffer + m_size + 2, packet, len);
    }
    m_size += 2 + len;
    m_packet_count++;
    return true;
}

bool parseOpusFrame(const uint8_t* frame, size_t len, std::vector<OpusPacketView>& out_packets) {
    out_packets.clear();
    if (!frame || len < 2) return false;
    if (frame[0] != CHANNEL_MIC) return false;
    if ((frame[1] & FLAG_OPUS) == 0) return false;

    size_t offset = 2;
    while (offset < len) {
        if (offset + 2 > len) return false; // truncated length prefix
        uint16_t pkt_len = static_cast<uint16_t>(frame[offset]) |
                          (static_cast<uint16_t>(frame[offset + 1]) << 8);
        offset += 2;
        if (offset + pkt_len > len) return false; // truncated packet
        out_packets.push_back({frame + offset, pkt_len});
        offset += pkt_len;
    }
    return true;
}

bool parsePcmFrame(const uint8_t* frame, size_t len, const uint8_t*& out_pcm, size_t& out_pcm_len) {
    out_pcm = nullptr;
    out_pcm_len = 0;
    if (!frame || len < 2) return false;
    if (frame[0] != CHANNEL_MIC) return false;
    if ((frame[1] & FLAG_OPUS) != 0) return false;

    out_pcm = frame + 2;
    out_pcm_len = len - 2;
    return true;
}

size_t buildPcmFrame(uint8_t* buffer, size_t capacity, const uint8_t* pcm, size_t pcm_len) {
    if (!buffer || capacity < 2 + pcm_len) return 0;
    buffer[0] = CHANNEL_MIC;
    buffer[1] = FLAG_PCM;
    if (pcm && pcm_len > 0) {
        std::memcpy(buffer + 2, pcm, pcm_len);
    }
    return 2 + pcm_len;
}

} // namespace Services
