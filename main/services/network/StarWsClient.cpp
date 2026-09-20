#include "services/network/StarWsClient.h"
#include "common/thread_config.h"
#include "esp_log.h"
#include <cstring>
#include <algorithm>

namespace Services {

StarWsClient& StarWsClient::getInstance() {
    static StarWsClient s_instance;
    return s_instance;
}

StarWsClient::StarWsClient()
    : ReactorTask({
        .name       = "star_ws_cli",
        .stack_size = 4096,
        .priority   = ThreadConfig::Priority::LOW,
        .core_id    = ThreadConfig::CORE_NETWORK,
        .interest   = COMP::ALL
    }) {
    m_client_mutex = xSemaphoreCreateMutex();
}

StarWsClient::~StarWsClient() {
    stop();
    if (m_client_mutex) {
        vSemaphoreDelete(m_client_mutex);
        m_client_mutex = nullptr;
    }
}

bool StarWsClient::begin() {
    auto snap = EmbeddedSysDb::getInstance().snapshot();
    if (snap.system.wifi_connected) {
        connectToServer(snap.system.server_ip);
    }
    return true;
}

void StarWsClient::stop() {
    disconnectFromServer();
}

void StarWsClient::onStateChanged(ComponentMask changed, const SystemState& snap) {
    if (changed & COMP::SYSTEM) {
        if (snap.system.wifi_connected && !m_client) {
            connectToServer(snap.system.server_ip);
        } else if (!snap.system.wifi_connected && m_client) {
            disconnectFromServer();
        }
    }

    if (m_task_handle) {
        xTaskNotifyGive(m_task_handle);
    }
}

void StarWsClient::connectToServer(const char* server_ip) {
    xSemaphoreTake(m_client_mutex, portMAX_DELAY);

    if (m_client) {
        xSemaphoreGive(m_client_mutex);
        return;
    }

    if (!server_ip || server_ip[0] == '\0') {
        server_ip = "192.168.1.18";
    }

    if (strncmp(server_ip, "ws://", 5) == 0 || strncmp(server_ip, "wss://", 6) == 0) {
        m_server_uri = server_ip;
    } else {
        m_server_uri = "ws://" + std::string(server_ip) + ":8765/api/star/ws";
    }

    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = m_server_uri.c_str();
    ws_cfg.buffer_size = 4096;
    ws_cfg.reconnect_timeout_ms = 5000;
    ws_cfg.network_timeout_ms = 5000;
    ws_cfg.task_stack = 4096;
    ws_cfg.task_prio = ThreadConfig::Priority::LOW;
    ws_cfg.task_core_id = ThreadConfig::CORE_NETWORK;
    ws_cfg.task_core_id_set = true;

    m_client = esp_websocket_client_init(&ws_cfg);
    if (!m_client) {
        ESP_LOGE(TAG, "Failed to initialize esp_websocket_client for %s", m_server_uri.c_str());
        xSemaphoreGive(m_client_mutex);
        return;
    }

    esp_websocket_register_events(m_client, WEBSOCKET_EVENT_ANY, websocketEventHandler, this);

    esp_err_t err = esp_websocket_client_start(m_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(m_client);
        m_client = nullptr;
    } else {
        ESP_LOGI(TAG, "Connecting to Host STAR daemon at %s...", m_server_uri.c_str());
    }

    xSemaphoreGive(m_client_mutex);
}

void StarWsClient::disconnectFromServer() {
    xSemaphoreTake(m_client_mutex, portMAX_DELAY);
    if (m_client) {
        ESP_LOGI(TAG, "Disconnecting from STAR Host daemon...");
        esp_websocket_client_stop(m_client);
        esp_websocket_client_destroy(m_client);
        m_client = nullptr;
        m_connected = false;
        m_synced = false;
    }
    xSemaphoreGive(m_client_mutex);
}

void StarWsClient::sendFrame(const std::vector<uint8_t>& frame) {
    xSemaphoreTake(m_client_mutex, portMAX_DELAY);
    if (!m_client || !m_connected) {
        xSemaphoreGive(m_client_mutex);
        return;
    }

    int ret = esp_websocket_client_send_bin(
        m_client, reinterpret_cast<const char*>(frame.data()), frame.size(), pdMS_TO_TICKS(2000));
    if (ret < 0) {
        ESP_LOGW(TAG, "Failed to send binary frame (%d)", ret);
    }
    xSemaphoreGive(m_client_mutex);
}

void StarWsClient::websocketEventHandler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
    auto* self = static_cast<StarWsClient*>(handler_args);
    auto* data = static_cast<esp_websocket_event_data_t*>(event_data);

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected to STAR Host daemon at %s", self->m_server_uri.c_str());
            self->m_connected = true;
            self->m_synced = false;
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Disconnected from STAR Host daemon");
            self->m_connected = false;
            self->m_synced = false;
            break;

        case WEBSOCKET_EVENT_DATA:
            if (data->data_len > 0) {
                self->handleWsData(reinterpret_cast<const uint8_t*>(data->data_ptr), data->data_len);
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error occurred");
            break;

        default:
            break;
    }
}

void StarWsClient::handleWsData(const uint8_t* data, size_t len) {
    if (len < StarProtocol::HEADER_SIZE) return;

    uint8_t msg_type = data[0];
    uint16_t payload_len = StarProtocol::readU16BE(&data[1]);
    if (len < StarProtocol::HEADER_SIZE + payload_len) return;

    const uint8_t* payload = data + StarProtocol::HEADER_SIZE;
    auto& sysdb = EmbeddedSysDb::getInstance();

    switch (static_cast<StarProtocol::MsgType>(msg_type)) {
        case StarProtocol::MsgType::REQ_CATCHUP: {
            if (payload_len < 4) return;
            uint32_t since_seq = StarProtocol::readU32LE(payload);
            std::vector<WalRecordEntry> records;
            WalQueryResult qres = sysdb.getWalRecordsSince(since_seq, records);

            if (qres == WalQueryResult::UP_TO_DATE) {
                m_last_sent_seq = since_seq;
                m_synced = true;
                std::vector<WalRecordEntry> empty_recs;
                sendFrame(StarProtocol::buildWalBatchFrame(empty_recs));
                return;
            }

            if (qres == WalQueryResult::SUCCESS) {
                m_last_sent_seq = records.empty() ? since_seq : records.back().seq;
                m_synced = true;
                sendFrame(StarProtocol::buildWalBatchFrame(records));
                return;
            }

            // Snapshot required
            ESP_LOGI(TAG, "Host requested full state snapshot (requested seq %lu, current head %lu)",
                     (unsigned long)since_seq, (unsigned long)sysdb.walHeadSeq());

            std::vector<WalRecordEntry> snapshot;
            uint32_t head_seq = sysdb.exportSnapshot(snapshot);
            m_last_sent_seq = head_seq;
            m_synced = true;

            // 1. SNAPSHOT_START
            sendFrame(StarProtocol::buildSnapshotStartFrame(head_seq, static_cast<uint16_t>(snapshot.size())));

            // 2. Stream individual fields
            for (const auto& r : snapshot) {
                sendFrame(StarProtocol::buildSnapshotFieldFrame(
                    r.component_id, r.field_tag, r.value.data(), static_cast<uint8_t>(r.value.size())));
            }

            // 3. SNAPSHOT_END
            sendFrame(StarProtocol::buildSnapshotEndFrame(head_seq));
            break;
        }

        case StarProtocol::MsgType::CMD_SET_FIELD: {
            if (payload_len < 3) return;
            uint8_t comp_id = payload[0];
            uint8_t field_tag = payload[1];
            uint8_t val_len = payload[2];
            if (payload_len < 3 + val_len) return;

            const uint8_t* val_ptr = payload + 3;
            WriteResult wres = sysdb.processRemoteWrite(
                static_cast<ComponentId>(comp_id), field_tag, val_ptr, val_len);

            sendFrame(StarProtocol::buildAckFrame(wres, sysdb.walHeadSeq()));
            break;
        }

        case StarProtocol::MsgType::CMD_EXEC_ACTION: {
            if (payload_len < 10) return;
            uint8_t cmd_id = payload[0];
            uint32_t nonce = StarProtocol::readU32LE(&payload[1]);
            uint32_t param = StarProtocol::readU32LE(&payload[5]);
            uint8_t data_len = payload[9];
            if (payload_len < 10 + data_len) return;

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

            sendFrame(StarProtocol::buildAckFrame(wres, sysdb.walHeadSeq()));
            break;
        }

        default:
            ESP_LOGW(TAG, "Unhandled msg_type 0x%02x from host", msg_type);
            break;
    }
}

void StarWsClient::run() {
    while (m_running) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!m_running) break;

        if (!m_client || !m_connected || !m_synced) {
            continue;
        }

        auto& sysdb = EmbeddedSysDb::getInstance();
        uint32_t head = sysdb.walHeadSeq();
        if (m_last_sent_seq >= head) {
            continue;
        }

        std::vector<WalRecordEntry> records;
        WalQueryResult qres = sysdb.getWalRecordsSince(m_last_sent_seq, records, 64);
        if (qres == WalQueryResult::SUCCESS && !records.empty()) {
            sendFrame(StarProtocol::buildWalBatchFrame(records));
            m_last_sent_seq = records.back().seq;
        }
    }
}

} // namespace Services
