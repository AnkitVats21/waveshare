#include "services/network/NetStats.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include <atomic>

namespace {

std::atomic<uint32_t> s_rx{0};
std::atomic<uint32_t> s_tx{0};
netif_input_fn s_inputFn = nullptr;
netif_linkoutput_fn s_outputFn = nullptr;
uint32_t s_lastRx = 0;
uint32_t s_lastTx = 0;
esp_timer_handle_t s_timer = nullptr;

// Runs on the Wi-Fi task for every received frame: one relaxed add, no lock.
err_t rxHook(struct pbuf* p, struct netif* n) {
    const uint16_t len = p->tot_len;  // read first: the stack may free p
    s_rx.fetch_add(len, std::memory_order_relaxed);
    return s_inputFn(p, n);
}

err_t txHook(struct netif* n, struct pbuf* p) {
    s_tx.fetch_add(p->tot_len, std::memory_order_relaxed);
    return s_outputFn(n, p);
}

// Publishes the totals to the sysdb (read-only fields) so any reader, the
// dashboard push, the REST API or the screen, gets them from a snapshot.
void publish(void*) {
    const uint32_t rx = s_rx.load(std::memory_order_relaxed);
    const uint32_t tx = s_tx.load(std::memory_order_relaxed);
    if (rx == s_lastRx && tx == s_lastTx) return;  // idle: no change bits
    s_lastRx = rx;
    s_lastTx = tx;
    EmbeddedSysDb::getInstance().mutate([rx, tx](SystemState& s) {
        s.system.net_rx_bytes = rx;
        s.system.net_tx_bytes = tx;
    });
}

}  // namespace

namespace NetStats {

void install(esp_netif_t* sta) {
    if (!sta) return;
    if (!s_timer) {
        esp_timer_create_args_t args = {};
        args.callback = publish;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "net_stats";
        if (esp_timer_create(&args, &s_timer) == ESP_OK) esp_timer_start_periodic(s_timer, 1000000);
    }
    auto* n = static_cast<struct netif*>(esp_netif_get_netif_impl(sta));
    // lwIP fills these in when Wi-Fi starts (netif_add), not when the netif
    // is created; a restart of the interface resets them, so hook again.
    if (!n || !n->input || !n->linkoutput) return;
    if (n->input == rxHook && n->linkoutput == txHook) return;
    s_inputFn = n->input;
    s_outputFn = n->linkoutput;
    n->input = rxHook;
    n->linkoutput = txHook;
}

uint32_t rxBytes() { return s_rx.load(std::memory_order_relaxed); }
uint32_t txBytes() { return s_tx.load(std::memory_order_relaxed); }

}  // namespace NetStats
