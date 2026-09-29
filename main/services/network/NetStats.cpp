#include "services/network/NetStats.h"
#include "esp_netif_net_stack.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include <atomic>

namespace {

std::atomic<uint32_t> s_rx{0};
std::atomic<uint32_t> s_tx{0};
netif_input_fn s_inputFn = nullptr;
netif_linkoutput_fn s_outputFn = nullptr;

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

}  // namespace

namespace NetStats {

void install(esp_netif_t* sta) {
    if (!sta || s_inputFn) return;
    auto* n = static_cast<struct netif*>(esp_netif_get_netif_impl(sta));
    if (!n || !n->input || !n->linkoutput) return;
    s_inputFn = n->input;
    s_outputFn = n->linkoutput;
    n->input = rxHook;
    n->linkoutput = txHook;
}

uint32_t rxBytes() { return s_rx.load(std::memory_order_relaxed); }
uint32_t txBytes() { return s_tx.load(std::memory_order_relaxed); }

}  // namespace NetStats
