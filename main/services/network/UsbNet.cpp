#include "services/network/UsbNet.h"
#include "services/network/NetStats.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "common/AppLogger.h"
#include "common/thread_config.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "tinyusb.h"
#include "tinyusb_net.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <cstring>
#include <strings.h>

namespace {

constexpr const char* NVS_NS = "net";
constexpr const char* NVS_KEY = "usb_mode";

esp_netif_t* s_netif = nullptr;
esp_timer_handle_t s_fallback = nullptr;
std::atomic<bool> s_has_ip{false};

// lwIP → USB. Runs on the tcpip task; tinyusb_net_send_sync copies the
// frame into an NTB before returning, so lwIP keeps ownership of buffer.
// When the NTBs are full it fails at once: retry briefly rather than drop
// the frame, since a drop costs TCP a retransmit timeout.
esp_err_t transmit(void*, void* buffer, size_t len) {
    for (int tries = 0; tries < 20; ++tries) {
        const esp_err_t err = tinyusb_net_send_sync(buffer, static_cast<uint16_t>(len), nullptr,
                                                    pdMS_TO_TICKS(100));
        if (err != ESP_FAIL) return err;
        vTaskDelay(1);
    }
    return ESP_FAIL;
}

void freeRx(void*, void* buffer) { heap_caps_free(buffer); }

// USB → lwIP. Runs on the TinyUSB task; the frame is only valid during the
// call, so copy it (to PSRAM, like the Wi-Fi driver's dynamic RX buffers).
esp_err_t receive(void* buffer, uint16_t len, void*) {
    if (!s_netif) return ESP_OK;
    void* copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) copy = heap_caps_malloc(len, MALLOC_CAP_8BIT);
    if (!copy) return ESP_ERR_NO_MEM;
    std::memcpy(copy, buffer, len);
    if (esp_netif_receive(s_netif, copy, len, copy) != ESP_OK) heap_caps_free(copy);
    return ESP_OK;
}

void onIpEvent(void*, esp_event_base_t, int32_t id, void* data) {
    if (id == IP_EVENT_ETH_GOT_IP) {
        auto* ev = static_cast<ip_event_got_ip_t*>(data);
        if (ev->esp_netif != s_netif) return;
        s_has_ip = true;
        if (s_fallback) esp_timer_stop(s_fallback);
        LOGI_WIFI("USB network up, IP " IPSTR " (gateway " IPSTR ")",
                  IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw));
        NetStats::install(s_netif);
        // The rest of the firmware waits for "wifi_connected"; here it means
        // the network is up, over whichever link.
        EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
            s.system.network_state = NetworkState::Connected;
            s.system.wifi_connected = true;
            s.system.ap_active = false;
        });
    } else if (id == IP_EVENT_ETH_LOST_IP) {
        auto* ev = static_cast<ip_event_got_ip_t*>(data);
        if (ev->esp_netif != s_netif) return;
        s_has_ip = false;
        LOGW_WIFI("USB network lost its address");
        EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
            s.system.network_state = NetworkState::Connecting;
            s.system.wifi_connected = false;
        });
    }
}

void fallBackToSerial(void*) {
    if (s_has_ip) return;
    LOGW_WIFI("No address over USB after %u s: switching back to serial + Wi-Fi",
              (unsigned)UsbNet::kFallbackSec);
    UsbNet::saveMode(UsbNet::Mode::Serial);
    esp_restart();
}

}  // namespace

namespace UsbNet {

Mode savedMode() {
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY, &v);
        nvs_close(h);
    }
    return v == static_cast<uint8_t>(Mode::Ethernet) ? Mode::Ethernet : Mode::Serial;
}

bool saveMode(Mode mode) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    const bool ok = nvs_set_u8(h, NVS_KEY, static_cast<uint8_t>(mode)) == ESP_OK &&
                    nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

const char* modeName(Mode mode) { return mode == Mode::Ethernet ? "ethernet" : "serial"; }

bool parseMode(const char* name, Mode& out) {
    if (!name) return false;
    if (strcasecmp(name, "serial") == 0) { out = Mode::Serial; return true; }
    if (strcasecmp(name, "ethernet") == 0) { out = Mode::Ethernet; return true; }
    return false;
}

bool begin() {
    // The lwIP side gets the chip's Ethernet MAC; the PC's end of the link
    // gets the same address with the locally administered bit set.
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_ETH);

    tinyusb_config_t usb_cfg = {};
    usb_cfg.port = TINYUSB_PORT_FULL_SPEED_0;
    usb_cfg.phy.vbus_monitor_io = -1;
    usb_cfg.task.size = 4096;
    usb_cfg.task.priority = 19;  // above lwIP (18), as the Wi-Fi driver's task is
    usb_cfg.task.xCoreID = ThreadConfig::CORE_NETWORK;
    if (tinyusb_driver_install(&usb_cfg) != ESP_OK) {
        LOGW_WIFI("TinyUSB install failed");
        return false;
    }

    tinyusb_net_config_t net_cfg = {};
    std::memcpy(net_cfg.mac_addr, mac, sizeof(mac));
    net_cfg.mac_addr[0] |= 0x02;
    net_cfg.on_recv_callback = receive;
    if (tinyusb_net_init(&net_cfg) != ESP_OK) {
        LOGW_WIFI("USB network class init failed");
        return false;
    }

    // An Ethernet-style netif with a DHCP client. The "ETH_DEF" key makes
    // mDNS announce on it like on a real Ethernet port.
    esp_netif_inherent_config_t base = *ESP_NETIF_BASE_DEFAULT_ETH;
    base.if_desc = "usb";
    esp_netif_config_t cfg = {};
    cfg.base = &base;
    cfg.stack = ESP_NETIF_NETSTACK_DEFAULT_ETH;
    s_netif = esp_netif_new(&cfg);
    if (!s_netif) return false;

    esp_netif_driver_ifconfig_t drv = {};
    drv.handle = s_netif;  // unused, but must be non-null
    drv.transmit = transmit;
    drv.driver_free_rx_buffer = freeRx;
    esp_netif_set_driver_config(s_netif, &drv);
    esp_netif_set_mac(s_netif, mac);

    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, onIpEvent, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, onIpEvent, nullptr);

    esp_netif_action_start(s_netif, nullptr, 0, nullptr);
    esp_netif_action_connected(s_netif, nullptr, 0, nullptr);  // starts DHCP

    esp_timer_create_args_t args = {};
    args.callback = fallBackToSerial;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "usbnet_fallback";
    if (esp_timer_create(&args, &s_fallback) == ESP_OK) {
        esp_timer_start_once(s_fallback, uint64_t(kFallbackSec) * 1000000ULL);
    }

    EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
        s.system.network_state = NetworkState::Connecting;
        s.system.wifi_connected = false;
        s.system.ap_active = false;
        std::strncpy(s.system.wifi_ssid, "USB", sizeof(s.system.wifi_ssid) - 1);
    });
    LOGI_WIFI("USB network mode: Wi-Fi off, waiting for an address from the PC");
    return true;
}

std::string ip() {
    if (!s_netif || !s_has_ip) return {};
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(s_netif, &info) != ESP_OK) return {};
    char buf[16];
    esp_ip4addr_ntoa(&info.ip, buf, sizeof(buf));
    return buf;
}

}  // namespace UsbNet
