#include "services/network/WifiSniff.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include <atomic>
#include <cstring>

namespace {

WifiSniff::Stats s_stats = {};
std::atomic<bool> s_running{false};
uint8_t s_mac[6];
esp_timer_handle_t s_timer = nullptr;

// Block Ack action frame (category 3): ADDBA request (action 0) carries
// the TID in bits 2-5 of its parameter set, DELBA (action 2) in bits 12-15.
void onAction(const uint8_t* h, unsigned len) {
    if (len < 24 + 6 + 4 || h[24] != 3) return;
    WifiSniff::Stats& s = s_stats;
    if (h[25] == 0) {
        ++s.addba_req[((h[27] | h[28] << 8) >> 2) & 0x07];
    } else if (h[25] == 2) {
        ++s.delba[((h[26] | h[27] << 8) >> 12) & 0x07];
    }
}

// Runs on the Wi-Fi task for every data and management frame the radio hears: only the
// station's own frames are counted, a few adds each.
void onFrame(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (!s_running.load(std::memory_order_relaxed)) return;
    if (type != WIFI_PKT_DATA && type != WIFI_PKT_MGMT) return;
    const auto* pkt = static_cast<const wifi_promiscuous_pkt_t*>(buf);
    const wifi_pkt_rx_ctrl_t& rx = pkt->rx_ctrl;
    const uint8_t* h = pkt->payload;
    if (rx.sig_len < 24 + 4 || std::memcmp(h + 4, s_mac, 6) != 0) return;  // addr1: receiver
    if (type == WIFI_PKT_MGMT) {
        if (h[0] == 0xD0) onAction(h, rx.sig_len);  // subtype 13: action
        return;
    }

    WifiSniff::Stats& s = s_stats;
    ++s.frames;
    s.rssi_sum += rx.rssi;
    if (rx.rx_state) ++s.rx_errors;
    if (rx.sig_mode == 1) {
        if (rx.mcs < 16) ++s.ht_mcs[rx.mcs];
    } else if (rx.sig_mode == 0) {
        ++s.legacy[rx.rate & 31];
    }
    const bool qos = (h[0] & 0xF0) == 0x80;  // data subtype 8: QoS data
    if (!qos) {
        ++s.non_qos;
        return;
    }
    const bool four_addr = (h[1] & 0x03) == 0x03;
    const unsigned qc_off = four_addr ? 30 : 24;
    if (rx.sig_len < qc_off + 2 + 4) return;
    WifiSniff::PerTid& t = s.tid[h[qc_off] & 0x07];
    ++t.frames;
    t.bytes += rx.sig_len;
    if (h[1] & 0x08) ++t.retries;
    if (rx.aggregation) ++t.aggregated;
}

void stop(void*) {
    s_running = false;
    esp_wifi_set_promiscuous(false);
    s_stats.running = false;
}

}  // namespace

namespace WifiSniff {

bool start(uint32_t seconds) {
    if (s_running || seconds < 1 || seconds > 30) return false;
    if (!s_timer) {
        esp_timer_create_args_t args = {};
        args.callback = stop;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "wifi_sniff";
        if (esp_timer_create(&args, &s_timer) != ESP_OK) return false;
    }
    if (esp_wifi_get_mac(WIFI_IF_STA, s_mac) != ESP_OK) return false;
    s_stats = {};
    s_stats.running = true;
    s_stats.seconds = seconds;
    wifi_promiscuous_filter_t filter = {};
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_MGMT;
    if (esp_wifi_set_promiscuous_filter(&filter) != ESP_OK ||
        esp_wifi_set_promiscuous_rx_cb(onFrame) != ESP_OK) {
        s_stats.running = false;
        return false;
    }
    s_running = true;
    if (esp_wifi_set_promiscuous(true) != ESP_OK) {
        s_running = false;
        s_stats.running = false;
        return false;
    }
    esp_timer_start_once(s_timer, uint64_t(seconds) * 1000000ULL);
    return true;
}

Stats stats() { return s_stats; }

}  // namespace WifiSniff
