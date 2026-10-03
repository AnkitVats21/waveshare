#include "services/hub/HubLink.h"

#include "services/http/ControlChannel.h"
#include "services/storage/SystemDatabase.h"
#include "credentials/Credentials.h"
#include "core_sysdb/EmbeddedSysDb.h"
#include "common/thread_config.h"

#include <ArduinoJson.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <mdns.h>
#include <algorithm>
#include <cstring>

namespace Services {

HubLink& HubLink::getInstance() {
    static HubLink s_instance;
    return s_instance;
}

void HubLink::start() {
    if (m_task) return;
    m_rx_queue = xQueueCreate(RX_QUEUE_LEN, sizeof(std::string*));
    if (!m_rx_queue ||
        xTaskCreatePinnedToCoreWithCaps(taskEntry, "hub_link", TASK_STACK, this, ThreadConfig::LOW, &m_task,
                                        ThreadConfig::CORE_NETWORK, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "Could not start the hub link task");
        m_task = nullptr;
    }
}

void HubLink::reconfigure() {
    if (m_task) xTaskNotify(m_task, BIT_RECONFIGURE, eSetBits);
}

HubLink::Status HubLink::status() {
    Status s;
    s.up = m_up.load();
    s.connected = m_connected.load();
    std::lock_guard<std::mutex> lock(m_status_mutex);
    s.from_mdns = m_from_mdns;
    s.url = m_url;
    s.hub_version = m_hub_version;
    s.last_error = m_last_error;
    return s;
}

void HubLink::setError(const char* error) {
    std::lock_guard<std::mutex> lock(m_status_mutex);
    m_last_error = error ? error : "";
}

bool HubLink::sendText(const std::string& text) {
    std::lock_guard<std::mutex> lock(m_client_mutex);
    if (!m_client || !m_up.load()) return false;
    const TickType_t timeout = pdMS_TO_TICKS(1000);
    if (text.size() <= TEXT_FRAGMENT) {
        return esp_websocket_client_send_text(m_client, text.data(), text.size(), timeout) ==
               static_cast<int>(text.size());
    }
    // One message in fragments (a voice session's setup is ~20 KB).
    const char* p = text.data();
    size_t left = text.size();
    if (esp_websocket_client_send_text_partial(m_client, p, TEXT_FRAGMENT, timeout) < 0) return false;
    p += TEXT_FRAGMENT;
    left -= TEXT_FRAGMENT;
    while (left > 0) {
        size_t n = std::min(left, TEXT_FRAGMENT);
        if (esp_websocket_client_send_cont_msg(m_client, p, n, timeout) < 0) return false;
        p += n;
        left -= n;
    }
    return esp_websocket_client_send_fin(m_client, timeout) >= 0;
}

bool HubLink::sendBinary(const uint8_t* frame, size_t len, uint32_t timeout_ms) {
    std::lock_guard<std::mutex> lock(m_client_mutex);
    if (!m_client || !m_up.load()) return false;
    return esp_websocket_client_send_bin(m_client, reinterpret_cast<const char*>(frame), len,
                                         pdMS_TO_TICKS(timeout_ms)) == static_cast<int>(len);
}

// The voice and music listeners hear every change of the link, once.
void HubLink::reportLink() {
    bool up = m_up.load();
    if (up == m_reported_up) return;
    m_reported_up = up;
    if (m_voice.onLinkChange) m_voice.onLinkChange(up, m_voice.ctx);
    if (m_music.onLinkChange) m_music.onLinkChange(up, m_music.ctx);
}

// ─────────────────────────────────────────────────────────────────────────────
// Task
// ─────────────────────────────────────────────────────────────────────────────

void HubLink::taskEntry(void* arg) {
    static_cast<HubLink*>(arg)->run();
}

void HubLink::run() {
    ControlChannel::getInstance().setRelay(
        [](const std::string& text, void* ctx) { return static_cast<HubLink*>(ctx)->sendText(text); }, this);

    int64_t retry_at_us = 0;
    for (;;) {
        int64_t now = esp_timer_get_time();
        uint32_t wait_ms = 5000;
        if (!m_client && retry_at_us > now) {
            wait_ms = std::min<int64_t>(wait_ms, (retry_at_us - now) / 1000 + 1);
        }
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(wait_ms));

        std::string* frame = nullptr;
        while (xQueueReceive(m_rx_queue, &frame, 0) == pdTRUE) {
            handleMessage(*frame);
            delete frame;
        }
        if (bits & BIT_RECONFIGURE) {
            ESP_LOGI(TAG, "Settings changed; reconnecting");
            close();
            m_rejected = false;
            retry_at_us = 0;
        }
        if (bits & BIT_DOWN) setDown();
        if ((bits & BIT_CONNECTED) && m_connected.load()) sendHello();
        reportLink();

        now = esp_timer_get_time();
        if (m_rejected) {
            // A wrong token stays wrong: try again much later (or on reconfigure).
            close();
            m_rejected = false;
            retry_at_us = now + REJECTED_RETRY_MS * 1000LL;
            continue;
        }
        if (m_client) {
            // A hub found by mDNS may have moved: look it up again.
            bool from_mdns;
            {
                std::lock_guard<std::mutex> lock(m_status_mutex);
                from_mdns = m_from_mdns;
            }
            if (from_mdns && !m_connected.load() && m_down_since_us && now - m_down_since_us > REDISCOVER_US) {
                ESP_LOGI(TAG, "Hub unreachable for a minute; looking it up again");
                close();
                retry_at_us = 0;
            }
            continue;
        }
        if (now < retry_at_us || !EmbeddedSysDb::getInstance().wifiConnected()) continue;

        std::string url = loadSettings().hub_url;
        bool from_mdns = url.empty();
        if (from_mdns) url = discover();
        {
            std::lock_guard<std::mutex> lock(m_status_mutex);
            m_url = url;
            m_from_mdns = from_mdns;
        }
        if (url.empty()) {
            setError("no hub found (mDNS _nexus-hub._tcp)");
            retry_at_us = now + RETRY_MS * 1000LL;
            continue;
        }
        if (!open(url)) retry_at_us = now + RETRY_MS * 1000LL;
    }
}

bool HubLink::open(const std::string& url) {
    esp_websocket_client_config_t cfg = {};
    cfg.uri = url.c_str();
    cfg.task_name = "hub_ws";
    cfg.task_stack = CLIENT_TASK_STACK;
    cfg.task_prio = ThreadConfig::NORMAL;
    cfg.task_core_id = ThreadConfig::CORE_NETWORK;
    cfg.task_core_id_set = true;
    cfg.buffer_size = CLIENT_BUFFER;
    cfg.reconnect_timeout_ms = RECONNECT_MS;
    cfg.network_timeout_ms = 5000;
    cfg.ping_interval_sec = 5;
    cfg.pingpong_timeout_sec = 10;

    esp_websocket_client_handle_t client = esp_websocket_client_init(&cfg);
    if (!client) {
        setError("client init failed");
        return false;
    }
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, onEvent, this);
    {
        std::lock_guard<std::mutex> lock(m_client_mutex);
        m_client = client;
    }
    m_down_since_us = esp_timer_get_time();
    if (esp_websocket_client_start(client) != ESP_OK) {
        setError("client start failed");
        close();
        return false;
    }
    ESP_LOGI(TAG, "Connecting to the hub at %s", url.c_str());
    return true;
}

void HubLink::close() {
    // Under the mutex, so no send is using the handle; the event handler
    // never takes it, so the client's task can stop meanwhile.
    std::lock_guard<std::mutex> lock(m_client_mutex);
    if (!m_client) return;
    m_up = false;
    m_connected = false;
    ControlChannel::getInstance().setRelayUp(false);
    reportLink();
    esp_websocket_client_destroy(m_client);
    m_client = nullptr;
    m_rx.clear();
    m_rx_dropping = false;
    m_rx_binary = false;
}

void HubLink::setDown() {
    if (!m_down_since_us) m_down_since_us = esp_timer_get_time();
    ControlChannel::getInstance().setRelayUp(m_up.load());
}

std::string HubLink::discover() {
    mdns_result_t* results = nullptr;
    if (mdns_query_ptr("_nexus-hub", "_tcp", 3000, 4, &results) != ESP_OK || !results) return "";
    std::string url;
    for (mdns_result_t* r = results; r && url.empty(); r = r->next) {
        if (!r->port) continue;
        for (mdns_ip_addr_t* a = r->addr; a; a = a->next) {
            if (a->addr.type != ESP_IPADDR_TYPE_V4) continue;
            std::string path = DEFAULT_PATH;
            for (size_t i = 0; i < r->txt_count; ++i) {
                if (r->txt[i].key && strcmp(r->txt[i].key, "path") == 0 && r->txt[i].value) {
                    path = r->txt[i].value;
                }
            }
            char host[24];
            snprintf(host, sizeof(host), IPSTR ":%u", IP2STR(&a->addr.u_addr.ip4), (unsigned)r->port);
            url = std::string("ws://") + host + path;
            break;
        }
    }
    mdns_query_results_free(results);
    return url;
}

void HubLink::sendHello() {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char id[13];
    snprintf(id, sizeof(id), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    const esp_app_desc_t* app = esp_app_get_description();

    JsonDocument doc;
    doc["type"] = "hello";
    doc["proto"] = PROTOCOL_VERSION;
    doc["id"] = id;
    doc["name"] = "Nexus";
    doc["kind"] = "esp32-s3";
    doc["fw"] = app ? app->version : "unknown";
    doc["token"] = credentials::hubToken();
    // Roles (nexus-orbit docs/PROTOCOL.md section 2).
    JsonObject roles = doc["roles"].to<JsonObject>();
    JsonObject voice = roles["voice"].to<JsonObject>();
    JsonArray up = voice["up"].to<JsonArray>();
    if (s_uplink_codec.load() != 1) {
        up.add("opus16");
    }
    up.add("pcm16");
    voice["down"].to<JsonArray>().add("pcm24");
    roles["state"]["http_port"] = CONFIG_WAVESHARE_HTTP_FILE_SERVER_PORT;
    if (s_music_control.load() != 1) {
        JsonObject player = roles["player"].to<JsonObject>();
        player["url"] = true;
        player["resolves"] = false;
        player["queue_copy"] = true;
        roles["controller"] = true;
    }
    std::string out;
    serializeJson(doc, out);

    std::lock_guard<std::mutex> lock(m_client_mutex);
    if (m_client && esp_websocket_client_send_text(m_client, out.data(), out.size(), pdMS_TO_TICKS(2000)) < 0) {
        ESP_LOGW(TAG, "Hello not sent");
    }
}

void HubLink::handleMessage(const std::string& text) {
    JsonDocument doc;
    if (deserializeJson(doc, text)) {
        ESP_LOGW(TAG, "Unreadable message (%u bytes)", (unsigned)text.size());
        return;
    }
    const char* type = doc["type"] | "";
    if (type[0] == '\0') {
        if (doc["cmd"].is<const char*>()) {
            ControlChannel::getInstance().handleRelayCommand(text.data(), text.size());
        }
        return;
    }
    if (strcmp(type, "welcome") == 0) {
        {
            std::lock_guard<std::mutex> lock(m_status_mutex);
            m_hub_version = doc["hub"] | "";
            m_last_error.clear();
        }
        m_down_since_us = 0;
        // voice: {"up": "opus"|"pcm", "down": "pcm", ...}, or false without Gemini on the hub.
        const char* up_codec = doc["voice"]["up"] | "";
        bool voice = doc["voice"].is<JsonObjectConst>() && strcmp(doc["voice"]["down"] | "", "pcm") == 0 &&
                     (strcmp(up_codec, "pcm") == 0 || strcmp(up_codec, "opus") == 0);
        m_hub_voice = voice;
        m_hub_voice_opus = voice && strcmp(up_codec, "opus") == 0 && (s_uplink_codec.load() != 1);
        m_hub_music = (doc["music"] | false) && s_music_control.load() != 1;
        m_up = true;
        ControlChannel::getInstance().setRelayUp(true);
        ESP_LOGI(TAG, "Hub link up (%s, voice %s [up=%s], music %s)", doc["hub"] | "?",
                 voice ? (m_hub_voice_opus.load() ? "opus" : "pcm") : "no",
                 up_codec, m_hub_music.load() ? "yes" : "no");
    } else if (strcmp(type, "rejected") == 0) {
        const char* reason = doc["reason"] | "rejected";
        ESP_LOGW(TAG, "The hub refused the board: %s", reason);
        setError(reason);
        m_rejected = true;
    } else if (strncmp(type, "music_", 6) == 0) {
        if (m_music.onMessage) m_music.onMessage(text, m_music.ctx);
    } else if (m_voice.onMessage) {
        m_voice.onMessage(text, m_voice.ctx);
    } else {
        ESP_LOGD(TAG, "Ignoring '%s'", type);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Client events (esp_websocket_client's task)
// ─────────────────────────────────────────────────────────────────────────────

void HubLink::onEvent(void* arg, esp_event_base_t, int32_t id, void* data) {
    auto* self = static_cast<HubLink*>(arg);
    switch (id) {
        case WEBSOCKET_EVENT_CONNECTED:
            self->m_connected = true;
            self->m_rx.clear();
            self->m_rx_dropping = false;
            xTaskNotify(self->m_task, BIT_CONNECTED, eSetBits);
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
            if (self->m_connected.exchange(false) || self->m_up.load()) {
                ESP_LOGI(TAG, "Hub link down");
            }
            self->m_up = false;
            xTaskNotify(self->m_task, BIT_DOWN, eSetBits);
            break;

        case WEBSOCKET_EVENT_DATA: {
            auto* d = static_cast<esp_websocket_event_data_t*>(data);
            // Binary: straight to the voice listener, piece by piece (reply
            // audio isn't queued behind control messages).
            if (d->op_code == 0x2 || (d->op_code == 0x0 && d->payload_offset > 0 && self->m_rx_binary)) {
                if (d->payload_offset == 0) {
                    self->m_rx_binary = true;
                    self->m_rx_header = 0;
                }
                const auto* p = reinterpret_cast<const uint8_t*>(d->data_ptr);
                size_t len = d->data_len > 0 ? d->data_len : 0;
                bool start = false;
                while (self->m_rx_header < 2 && len > 0) {
                    if (self->m_rx_header == 0) self->m_rx_channel = *p;
                    ++self->m_rx_header;
                    ++p;
                    --len;
                    start = self->m_rx_header == 2;
                }
                if (self->m_rx_header == 2 && (len > 0 || start) && self->m_voice.onAudio) {
                    self->m_voice.onAudio(self->m_rx_channel, p, len, start, self->m_voice.ctx);
                }
                break;
            }
            if (d->op_code != 0x1 && !(d->op_code == 0x0 && d->payload_offset > 0)) break;  // text only
            self->m_rx_binary = false;
            if (d->payload_offset == 0) {
                self->m_rx.clear();
                self->m_rx_dropping = d->payload_len > static_cast<int>(MAX_RX_FRAME);
                if (self->m_rx_dropping) ESP_LOGW(TAG, "Dropping a %d-byte message", d->payload_len);
            }
            if (!self->m_rx_dropping && d->data_len > 0) self->m_rx.append(d->data_ptr, d->data_len);
            if (d->payload_offset + d->data_len < d->payload_len) break;   // more of this frame follows
            if (!self->m_rx_dropping && !self->m_rx.empty()) {
                auto* frame = new std::string(std::move(self->m_rx));
                if (xQueueSend(self->m_rx_queue, &frame, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "Message queue full; dropped one");
                    delete frame;
                } else {
                    xTaskNotify(self->m_task, BIT_RX, eSetBits);
                }
            }
            self->m_rx.clear();
            self->m_rx_dropping = false;
            break;
        }

        default:
            break;
    }
}

} // namespace Services
