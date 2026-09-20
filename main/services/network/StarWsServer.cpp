#include "services/network/StarWsServer.h"
#include "common/thread_config.h"
#include "esp_log.h"
#include <algorithm>
#include <cstring>

namespace Services {

StarWsServer& StarWsServer::getInstance() {
    static StarWsServer s_instance;
    return s_instance;
}

StarWsServer::StarWsServer()
    : ReactorTask({
        .name       = "star_ws_srv",
        .stack_size = 4096,
        .priority   = ThreadConfig::Priority::LOW,
        .core_id    = ThreadConfig::CORE_NETWORK,
        .interest   = COMP::ALL
    }) {
    m_clients_mutex = xSemaphoreCreateMutex();
    for (auto& c : m_clients) {
        c.fd = -1;
    }
}

StarWsServer::~StarWsServer() {
    unregisterHandler();
    if (m_clients_mutex) {
        vSemaphoreDelete(m_clients_mutex);
        m_clients_mutex = nullptr;
    }
}

bool StarWsServer::registerHandler(httpd_handle_t server) {
    if (!server) return false;
    m_server = server;

    httpd_uri_t ws_uri = {
        .uri                      = "/api/star/ws",
        .method                   = HTTP_GET,
        .handler                  = wsHandler,
        .user_ctx                 = this,
        .is_websocket             = true,
        .handle_ws_control_frames = true,
        .supported_subprotocol    = nullptr,
        .ws_post_handshake_cb     = nullptr
    };

    esp_err_t ret = httpd_register_uri_handler(server, &ws_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/star/ws handler: %s", esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(TAG, "Registered STAR WebSocket replication endpoint at /api/star/ws");
    return true;
}

void StarWsServer::unregisterHandler() {
    if (m_server) {
        httpd_unregister_uri_handler(m_server, "/api/star/ws", HTTP_GET);
        m_server = nullptr;
    }

    if (m_clients_mutex) {
        xSemaphoreTake(m_clients_mutex, portMAX_DELAY);
        for (auto& c : m_clients) {
            c.fd = -1;
            c.synced = false;
        }
        xSemaphoreGive(m_clients_mutex);
    }
}

void StarWsServer::onStateChanged(ComponentMask changed, const SystemState& snap) {
    (void)changed;
    (void)snap;
    if (m_task_handle) {
        xTaskNotifyGive(m_task_handle);
    }
}

void StarWsServer::run() {
    while (m_running) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!m_running) break;
        broadcastWalUpdates();
    }
}

esp_err_t StarWsServer::wsHandler(httpd_req_t* req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "STAR WebSocket handshake established (fd=%d)", httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    std::memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_BINARY;

    // Get length first
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame len query failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (ws_pkt.len == 0) {
        return ESP_OK;
    }

    std::vector<uint8_t> buf(ws_pkt.len);
    ws_pkt.payload = buf.data();
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame payload read failed: %s", esp_err_to_name(ret));
        return ret;
    }

    auto* self = static_cast<StarWsServer*>(req->user_ctx);
    if (!self) {
        self = &getInstance();
    }
    return self->processWsFrame(req, buf.data(), buf.size());
}

esp_err_t StarWsServer::processWsFrame(httpd_req_t* req, const uint8_t* data, size_t len) {
    if (len < StarProtocol::HEADER_SIZE) {
        ESP_LOGW(TAG, "Frame too small (%zu bytes)", len);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t msg_type = data[0];
    uint16_t payload_len = StarProtocol::readU16BE(&data[1]);
    if (len < StarProtocol::HEADER_SIZE + payload_len) {
        ESP_LOGW(TAG, "Truncated frame payload (have %zu, expected %u)",
                 len, StarProtocol::HEADER_SIZE + payload_len);
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t* payload = data + StarProtocol::HEADER_SIZE;
    int fd = httpd_req_to_sockfd(req);

    auto& sysdb = EmbeddedSysDb::getInstance();

    switch (static_cast<StarProtocol::MsgType>(msg_type)) {
        case StarProtocol::MsgType::REQ_CATCHUP: {
            if (payload_len < 4) {
                return ESP_ERR_INVALID_ARG;
            }
            uint32_t since_seq = StarProtocol::readU32LE(payload);
            std::vector<WalRecordEntry> records;
            WalQueryResult qres = sysdb.getWalRecordsSince(since_seq, records);

            if (qres == WalQueryResult::UP_TO_DATE) {
                trackClient(fd, since_seq, true);
                std::vector<WalRecordEntry> empty_recs;
                std::vector<uint8_t> batch = StarProtocol::buildWalBatchFrame(empty_recs);
                httpd_ws_frame_t resp{};
                resp.type = HTTPD_WS_TYPE_BINARY;
                resp.payload = batch.data();
                resp.len = batch.size();
                return httpd_ws_send_frame(req, &resp);
            }

            if (qres == WalQueryResult::SUCCESS) {
                uint32_t new_seq = records.empty() ? since_seq : records.back().seq;
                trackClient(fd, new_seq, true);

                std::vector<uint8_t> batch = StarProtocol::buildWalBatchFrame(records);
                httpd_ws_frame_t resp{};
                resp.type = HTTPD_WS_TYPE_BINARY;
                resp.payload = batch.data();
                resp.len = batch.size();
                return httpd_ws_send_frame(req, &resp);
            }

            // Snapshot required
            ESP_LOGI(TAG, "Client fd=%d requires full snapshot (requested seq %lu, current head %lu)",
                     fd, (unsigned long)since_seq, (unsigned long)sysdb.walHeadSeq());

            std::vector<WalRecordEntry> snapshot;
            uint32_t head_seq = sysdb.exportSnapshot(snapshot);
            trackClient(fd, head_seq, true);

            // 1. Send SNAPSHOT_START
            auto start_pkt = StarProtocol::buildSnapshotStartFrame(head_seq, static_cast<uint16_t>(snapshot.size()));
            httpd_ws_frame_t resp{};
            resp.type = HTTPD_WS_TYPE_BINARY;
            resp.payload = start_pkt.data();
            resp.len = start_pkt.size();
            esp_err_t err = httpd_ws_send_frame(req, &resp);
            if (err != ESP_OK) return err;

            // 2. Stream individual SNAPSHOT_FIELD frames
            for (const auto& r : snapshot) {
                auto field_pkt = StarProtocol::buildSnapshotFieldFrame(
                    r.component_id, r.field_tag, r.value.data(), static_cast<uint8_t>(r.value.size()));
                resp.payload = field_pkt.data();
                resp.len = field_pkt.size();
                err = httpd_ws_send_frame(req, &resp);
                if (err != ESP_OK) return err;
            }

            // 3. Send SNAPSHOT_END
            auto end_pkt = StarProtocol::buildSnapshotEndFrame(head_seq);
            resp.payload = end_pkt.data();
            resp.len = end_pkt.size();
            return httpd_ws_send_frame(req, &resp);
        }

        case StarProtocol::MsgType::CMD_SET_FIELD: {
            if (payload_len < 3) {
                return ESP_ERR_INVALID_ARG;
            }
            uint8_t comp_id = payload[0];
            uint8_t field_tag = payload[1];
            uint8_t val_len = payload[2];
            if (payload_len < 3 + val_len) {
                return ESP_ERR_INVALID_ARG;
            }
            const uint8_t* val_ptr = payload + 3;

            WriteResult wres = sysdb.processRemoteWrite(
                static_cast<ComponentId>(comp_id), field_tag, val_ptr, val_len);

            auto ack_pkt = StarProtocol::buildAckFrame(wres, sysdb.walHeadSeq());
            httpd_ws_frame_t resp{};
            resp.type = HTTPD_WS_TYPE_BINARY;
            resp.payload = ack_pkt.data();
            resp.len = ack_pkt.size();
            return httpd_ws_send_frame(req, &resp);
        }

        case StarProtocol::MsgType::CMD_EXEC_ACTION: {
            if (payload_len < 10) {
                return ESP_ERR_INVALID_ARG;
            }
            uint8_t cmd_id = payload[0];
            uint32_t nonce = StarProtocol::readU32LE(&payload[1]);
            uint32_t param = StarProtocol::readU32LE(&payload[5]);
            uint8_t data_len = payload[9];
            if (payload_len < 10 + data_len) {
                return ESP_ERR_INVALID_ARG;
            }
            const uint8_t* data_ptr = payload + 10;

            MediaPendingCommand cmd{};
            cmd.cmd = static_cast<MediaCmdId>(cmd_id);
            cmd.nonce = nonce;
            cmd.param = param;
            if (data_len > 0) {
                size_t copy_len = std::min<size_t>(data_len, sizeof(cmd.data) - 1);
                std::memcpy(cmd.data, data_ptr, copy_len);
                cmd.data[copy_len] = '\0';
            }

            WriteResult wres = sysdb.processRemoteWrite(
                ComponentId::MEDIA,
                TAG_MEDIA::pending_command,
                reinterpret_cast<const uint8_t*>(&cmd),
                sizeof(cmd));

            auto ack_pkt = StarProtocol::buildAckFrame(wres, sysdb.walHeadSeq());
            httpd_ws_frame_t resp{};
            resp.type = HTTPD_WS_TYPE_BINARY;
            resp.payload = ack_pkt.data();
            resp.len = ack_pkt.size();
            return httpd_ws_send_frame(req, &resp);
        }

        default:
            ESP_LOGW(TAG, "Unhandled msg_type 0x%02x from fd=%d", msg_type, fd);
            return ESP_ERR_NOT_SUPPORTED;
    }
}

void StarWsServer::trackClient(int fd, uint32_t last_seq, bool synced) {
    if (!m_clients_mutex || fd < 0) return;
    xSemaphoreTake(m_clients_mutex, portMAX_DELAY);

    // Look for existing client
    for (auto& c : m_clients) {
        if (c.fd == fd) {
            c.last_seq = last_seq;
            c.synced = synced;
            xSemaphoreGive(m_clients_mutex);
            return;
        }
    }

    // Allocate new slot
    for (auto& c : m_clients) {
        if (c.fd == -1) {
            c.fd = fd;
            c.last_seq = last_seq;
            c.synced = synced;
            ESP_LOGI(TAG, "Tracked new STAR client (fd=%d, seq=%lu)", fd, (unsigned long)last_seq);
            xSemaphoreGive(m_clients_mutex);
            return;
        }
    }

    ESP_LOGW(TAG, "Client list full (max %zu), cannot track fd=%d", MAX_STAR_CLIENTS, fd);
    xSemaphoreGive(m_clients_mutex);
}

void StarWsServer::updateClientSeq(int fd, uint32_t seq) {
    if (!m_clients_mutex || fd < 0) return;
    xSemaphoreTake(m_clients_mutex, portMAX_DELAY);
    for (auto& c : m_clients) {
        if (c.fd == fd) {
            c.last_seq = seq;
            break;
        }
    }
    xSemaphoreGive(m_clients_mutex);
}

void StarWsServer::removeClient(int fd) {
    if (!m_clients_mutex || fd < 0) return;
    xSemaphoreTake(m_clients_mutex, portMAX_DELAY);
    for (auto& c : m_clients) {
        if (c.fd == fd) {
            c.fd = -1;
            c.synced = false;
            ESP_LOGI(TAG, "Removed STAR client fd=%d", fd);
            break;
        }
    }
    xSemaphoreGive(m_clients_mutex);
}

void StarWsServer::broadcastWalUpdates() {
    if (!m_server || !m_clients_mutex) return;

    xSemaphoreTake(m_clients_mutex, portMAX_DELAY);

    auto& sysdb = EmbeddedSysDb::getInstance();
    uint32_t current_head = sysdb.walHeadSeq();

    for (auto& c : m_clients) {
        if (c.fd == -1) continue;

        // Check if socket is still a valid WebSocket
        if (httpd_ws_get_fd_info(m_server, c.fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
            ESP_LOGI(TAG, "Client fd=%d disconnected, pruning", c.fd);
            c.fd = -1;
            c.synced = false;
            continue;
        }

        if (!c.synced) continue;
        if (c.last_seq >= current_head) continue;

        std::vector<WalRecordEntry> records;
        WalQueryResult qres = sysdb.getWalRecordsSince(c.last_seq, records, 64);

        if (qres == WalQueryResult::SUCCESS && !records.empty()) {
            std::vector<uint8_t> batch = StarProtocol::buildWalBatchFrame(records);
            httpd_ws_frame_t pkt{};
            pkt.type = HTTPD_WS_TYPE_BINARY;
            pkt.payload = batch.data();
            pkt.len = batch.size();

            esp_err_t send_res = httpd_ws_send_frame_async(m_server, c.fd, &pkt);
            if (send_res == ESP_OK) {
                c.last_seq = records.back().seq;
            } else {
                ESP_LOGW(TAG, "Async send failed to fd=%d: %s, pruning", c.fd, esp_err_to_name(send_res));
                c.fd = -1;
                c.synced = false;
            }
        } else if (qres == WalQueryResult::SNAPSHOT_REQUIRED) {
            ESP_LOGW(TAG, "Client fd=%d lagged beyond ring buffer during streaming, marking unsynced", c.fd);
            c.synced = false;
        }
    }

    xSemaphoreGive(m_clients_mutex);
}

} // namespace Services
