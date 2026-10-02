#pragma once
#include <cstdint>

// Radio-level view of what the access point sends the board, for network
// tests: for a few seconds the driver's promiscuous mode reports every data
// frame addressed to the station, and these counters record how it arrived
// (PHY rate, aggregated or not, retry, priority queue). The station stays
// connected meanwhile.
namespace WifiSniff {

struct PerTid {
    uint32_t frames;
    uint32_t bytes;
    uint32_t retries;     // retry bit set: the AP is resending
    uint32_t aggregated;  // arrived inside an A-MPDU
};

struct Stats {
    bool running;
    uint32_t seconds;
    uint32_t frames;       // data frames addressed to the station
    uint32_t non_qos;      // data frames without a QoS header (no TID)
    uint32_t rx_errors;    // frames the radio flagged as bad
    PerTid tid[8];
    uint32_t ht_mcs[16];   // 802.11n frames by MCS (0-15)
    uint32_t legacy[32];   // 802.11b/g frames by the driver's rate code
    int32_t rssi_sum;
    // Block Ack (aggregation) management frames from the AP, by TID:
    // requests to start aggregating a queue, and teardowns.
    uint32_t addba_req[8];
    uint32_t delba[8];
};

// Starts a capture of `seconds` (1-30); false if one is running or the
// driver refused.
bool start(uint32_t seconds);

// The last capture's counters (running is true until it ends).
Stats stats();

}  // namespace WifiSniff
