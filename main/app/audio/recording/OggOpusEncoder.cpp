#include "app/audio/recording/OggOpusEncoder.h"

#include <cstring>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "opus.h"
#include "sd_storage/File.h"

namespace {
const char* TAG = "OggOpusEncoder";

constexpr size_t kMaxPacket = 1500;
constexpr size_t kBodyCap = 16 * 1024;
constexpr int kPacketsPerPage = 50;   // 1 s of 20 ms frames
constexpr size_t kBodyFlush = 8 * 1024;
constexpr int kPagesPerSync = 10;

// Ogg's CRC-32: polynomial 0x04c11db7, not reflected, initial value 0.
struct CrcTable {
    uint32_t v[256];
    constexpr CrcTable() : v() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t r = i << 24;
            for (int b = 0; b < 8; ++b) r = (r & 0x80000000u) ? (r << 1) ^ 0x04c11db7u : (r << 1);
            v[i] = r;
        }
    }
};
constexpr CrcTable kCrc;

uint32_t crcUpdate(uint32_t crc, const uint8_t* p, size_t n) {
    while (n--) crc = (crc << 8) ^ kCrc.v[((crc >> 24) ^ *p++) & 0xff];
    return crc;
}

void put16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = v >> (8 * i); }
void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = v >> (8 * i); }

void* psram(size_t bytes) { return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
} // namespace

OggOpusEncoder::~OggOpusEncoder() {
    if (m_enc) opus_encoder_destroy(m_enc);
    heap_caps_free(m_rs_out);
    heap_caps_free(m_frame);
    heap_caps_free(m_packet);
    heap_caps_free(m_body);
}

bool OggOpusEncoder::open(sd_storage::File& file, uint32_t input_rate, int channels) {
    m_file = &file;
    m_channels = channels;
    m_frame_size = m_cfg.encode_rate / 50;

    int err = OPUS_OK;
    // CELT only: the SILK (speech) layer's float build calls double-precision
    // math, which the S3 does in software; at 16 kHz it needed more than a
    // whole core even at complexity 0.
    m_enc = opus_encoder_create(m_cfg.encode_rate, channels, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
    if (err != OPUS_OK || !m_enc) {
        ESP_LOGE(TAG, "opus_encoder_create(%u Hz, %d ch) failed: %s",
                 (unsigned)m_cfg.encode_rate, channels, opus_strerror(err));
        m_enc = nullptr;
        return false;
    }
    opus_encoder_ctl(m_enc, OPUS_SET_BITRATE(m_cfg.bitrate));
    opus_encoder_ctl(m_enc, OPUS_SET_COMPLEXITY(m_cfg.complexity));
    opus_encoder_ctl(m_enc, OPUS_SET_SIGNAL(m_cfg.voice ? OPUS_SIGNAL_VOICE : OPUS_AUTO));
    opus_int32 lookahead = 0;
    opus_encoder_ctl(m_enc, OPUS_GET_LOOKAHEAD(&lookahead));
    m_pre_skip = static_cast<uint32_t>(lookahead) * (48000 / m_cfg.encode_rate);

    m_resample = input_rate != m_cfg.encode_rate;
    if (m_resample && !m_resampler.init(input_rate, m_cfg.encode_rate, channels)) {
        ESP_LOGE(TAG, "no resampler for %u -> %u Hz", (unsigned)input_rate, (unsigned)m_cfg.encode_rate);
        return false;
    }

    m_frame = static_cast<int16_t*>(psram(m_frame_size * channels * sizeof(int16_t)));
    m_packet = static_cast<uint8_t*>(psram(kMaxPacket));
    m_body = static_cast<uint8_t*>(psram(kBodyCap));
    if (!m_frame || !m_packet || !m_body) {
        ESP_LOGE(TAG, "out of PSRAM");
        return false;
    }
    m_serial = esp_random();

    // Identification header (RFC 7845 section 5.1), alone on the first page.
    uint8_t head[19];
    memcpy(head, "OpusHead", 8);
    head[8] = 1;
    head[9] = static_cast<uint8_t>(channels);
    put16(head + 10, static_cast<uint16_t>(m_pre_skip));
    put32(head + 12, input_rate);
    put16(head + 16, 0);  // output gain
    head[18] = 0;         // channel mapping family 0 (mono/stereo)
    uint8_t lace = sizeof(head);
    if (!writePage(0x02, 0, &lace, 1, head, sizeof(head))) return false;

    // Comment header, on its own page.
    static const char vendor[] = "nexus";
    uint8_t tags[8 + 4 + sizeof(vendor) - 1 + 4];
    memcpy(tags, "OpusTags", 8);
    put32(tags + 8, sizeof(vendor) - 1);
    memcpy(tags + 12, vendor, sizeof(vendor) - 1);
    put32(tags + 12 + sizeof(vendor) - 1, 0);  // no user comments
    lace = sizeof(tags);
    return writePage(0x00, 0, &lace, 1, tags, sizeof(tags));
}

bool OggOpusEncoder::writeSamples(const int16_t* data, size_t frame_count) {
    if (!m_enc || !data || frame_count == 0) return false;
    int64_t t0 = esp_timer_get_time();
    bool ok = true;
    if (m_resample) {
        size_t need = m_resampler.maxOutput(frame_count);
        if (need > m_rs_out_cap) {
            heap_caps_free(m_rs_out);
            m_rs_out = static_cast<int16_t*>(psram(need * m_channels * sizeof(int16_t)));
            m_rs_out_cap = m_rs_out ? need : 0;
            if (!m_rs_out) return false;
        }
        size_t n = m_resampler.process(data, frame_count, m_rs_out);
        m_input_frames += n;
        ok = feed(m_rs_out, n);
    } else {
        m_input_frames += frame_count;
        ok = feed(data, frame_count);
    }
    m_busy_us += esp_timer_get_time() - t0;
    return ok;
}

bool OggOpusEncoder::feed(const int16_t* pcm, size_t frames) {
    while (frames > 0) {
        size_t take = m_frame_size - m_frame_fill;
        if (take > frames) take = frames;
        memcpy(m_frame + m_frame_fill * m_channels, pcm, take * m_channels * sizeof(int16_t));
        m_frame_fill += take;
        pcm += take * m_channels;
        frames -= take;
        if (m_frame_fill == m_frame_size && !encodeFrame()) return false;
    }
    return true;
}

bool OggOpusEncoder::encodeFrame() {
    opus_int32 n = opus_encode(m_enc, m_frame, static_cast<int>(m_frame_size), m_packet, kMaxPacket);
    m_frame_fill = 0;
    if (n < 0) {
        ESP_LOGE(TAG, "opus_encode failed: %s", opus_strerror(n));
        return false;
    }
    addPacket(m_packet, static_cast<size_t>(n));
    // While finalizing, the last packets stay on the EOS page so its granule
    // can trim the padding.
    if (!m_finalizing && (m_page_packets >= kPacketsPerPage || m_body_len >= kBodyFlush || m_nseg > 255 - 7)) {
        // Granule: 48 kHz samples decoded through the last complete packet.
        return flushPage(false, static_cast<int64_t>(m_packets) * 960);
    }
    return true;
}

void OggOpusEncoder::addPacket(const uint8_t* data, size_t len) {
    // Lacing: 255-byte segments, then the remainder (0 if the length is a
    // multiple of 255, which terminates the packet).
    size_t left = len;
    while (left >= 255) {
        m_lacing[m_nseg++] = 255;
        left -= 255;
    }
    m_lacing[m_nseg++] = static_cast<uint8_t>(left);
    memcpy(m_body + m_body_len, data, len);
    m_body_len += len;
    ++m_page_packets;
    ++m_packets;
}

bool OggOpusEncoder::flushPage(bool eos, int64_t granule) {
    if (m_nseg == 0 && !eos) return true;
    bool ok = writePage(eos ? 0x04 : 0x00, granule, m_lacing, m_nseg, m_body, m_body_len);
    m_nseg = 0;
    m_body_len = 0;
    m_page_packets = 0;
    if (ok && ++m_pages_since_sync >= kPagesPerSync) {
        m_file->sync();  // keeps the directory entry's size current
        m_pages_since_sync = 0;
    }
    return ok;
}

bool OggOpusEncoder::writePage(uint8_t header_type, int64_t granule,
                               const uint8_t* lacing, size_t nseg, const uint8_t* body, size_t body_len) {
    uint8_t hdr[27 + 255];
    memcpy(hdr, "OggS", 4);
    hdr[4] = 0;
    hdr[5] = header_type;
    put64(hdr + 6, static_cast<uint64_t>(granule));
    put32(hdr + 14, m_serial);
    put32(hdr + 18, m_page_seq++);
    put32(hdr + 22, 0);
    hdr[26] = static_cast<uint8_t>(nseg);
    memcpy(hdr + 27, lacing, nseg);
    size_t hdr_len = 27 + nseg;

    uint32_t crc = crcUpdate(0, hdr, hdr_len);
    crc = crcUpdate(crc, body, body_len);
    put32(hdr + 22, crc);

    if (!m_file->writeAll(hdr, hdr_len) || !m_file->writeAll(body, body_len)) {
        ESP_LOGE(TAG, "page write failed");
        return false;
    }
    return true;
}

bool OggOpusEncoder::finalize() {
    if (!m_enc) return false;
    int64_t t0 = esp_timer_get_time();

    // The encoder holds back `lookahead` samples; push silence through so the
    // real audio's tail is encoded, then pad out the last frame.
    size_t lookahead = m_pre_skip / (48000 / m_cfg.encode_rate);
    size_t pad = lookahead + m_frame_size;
    bool ok = true;
    // Room for the padding packets (at most 7 more lacing values each).
    if (m_page_packets >= kPacketsPerPage - 4 || m_body_len >= kBodyFlush || m_nseg > 255 - 5 * 7) {
        ok = flushPage(false, static_cast<int64_t>(m_packets) * 960);
    }
    m_finalizing = true;
    while (ok && pad > 0) {
        size_t take = m_frame_size - m_frame_fill;
        if (take > pad) take = pad;
        memset(m_frame + m_frame_fill * m_channels, 0, take * m_channels * sizeof(int16_t));
        m_frame_fill += take;
        pad -= take;
        if (m_frame_fill == m_frame_size) ok = encodeFrame();
    }
    // End granule trims the padding: pre-skip plus the real audio.
    int64_t end = m_pre_skip + static_cast<int64_t>(m_input_frames) * (48000 / m_cfg.encode_rate);
    ok = flushPage(true, end) && ok;
    ok = m_file->sync() && ok;
    m_busy_us += esp_timer_get_time() - t0;
    return ok;
}
