#include "BtPlayerUart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include <cstring>

namespace btplayer {

BtPlayerUart& BtPlayerUart::getInstance() {
    static BtPlayerUart instance;
    return instance;
}

BtPlayerUart::~BtPlayerUart() {
    deinit();
}

bool BtPlayerUart::init(const Config& config) {
    if (m_running) {
        ESP_LOGW(TAG, "BtPlayerUart already initialized");
        return true;
    }

    m_port = static_cast<uart_port_t>(config.uart_num);

    if (!m_tx_mutex) {
        m_tx_mutex = xSemaphoreCreateMutex();
        if (!m_tx_mutex) {
            ESP_LOGE(TAG, "Failed to create TX mutex");
            return false;
        }
    }

    uart_config_t uart_cfg = {};
    uart_cfg.baud_rate           = config.baudrate;
    uart_cfg.data_bits           = UART_DATA_8_BITS;
    uart_cfg.parity              = UART_PARITY_DISABLE;
    uart_cfg.stop_bits           = UART_STOP_BITS_1;
    uart_cfg.flow_ctrl           = UART_HW_FLOWCTRL_DISABLE;
    uart_cfg.rx_flow_ctrl_thresh = 122;
    uart_cfg.source_clk          = UART_SCLK_DEFAULT;

    // Delete driver first if already allocated
    uart_driver_delete(m_port);

    esp_err_t err = uart_param_config(m_port, &uart_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        return false;
    }

    err = uart_set_pin(m_port, config.tx_pin, config.rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        return false;
    }

    err = uart_driver_install(m_port, config.rx_buffer_size, config.tx_buffer_size, 0, nullptr, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return false;
    }

    // Enable internal pull-up on RX pin to prevent floating-pin false start bits
    gpio_set_pull_mode(static_cast<gpio_num_t>(config.rx_pin), GPIO_PULLUP_ONLY);

    m_init_us = esp_timer_get_time();
    m_running = true;
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        rxTaskTrampoline,
        "bt_uart_rx",
        4096,
        this,
        8,
        &m_rx_task,
        1
    );

    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UART RX task");
        m_running = false;
        uart_driver_delete(m_port);
        return false;
    }

    ESP_LOGI(TAG, "BtPlayerUart initialized on UART%d (TX=%d, RX=%d @ %d baud)",
             m_port, config.tx_pin, config.rx_pin, config.baudrate);
    return true;
}

void BtPlayerUart::deinit() {
    if (!m_running) return;

    m_running = false;
    if (m_rx_task != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(50));
        m_rx_task = nullptr;
    }

    uart_driver_delete(m_port);

    if (m_tx_mutex != nullptr) {
        vSemaphoreDelete(m_tx_mutex);
        m_tx_mutex = nullptr;
    }
}

bool BtPlayerUart::sendFrame(MsgType type, const uint8_t* payload, size_t len) {
    if (!m_running || len > UART_MAX_PAYLOAD) {
        return false;
    }

    uint8_t frame[4 + UART_MAX_PAYLOAD + 1];
    frame[0] = UART_SYNC_1;
    frame[1] = UART_SYNC_2;
    frame[2] = static_cast<uint8_t>(type);
    frame[3] = static_cast<uint8_t>(len);
    if (payload != nullptr && len > 0) {
        std::memcpy(frame + 4, payload, len);
    }
    frame[4 + len] = crc8(frame + 2, 2 + len);

    if (xSemaphoreTake(m_tx_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "TX mutex timeout for msg 0x%02X", static_cast<uint8_t>(type));
        return false;
    }

    int written = uart_write_bytes(m_port, frame, 4 + len + 1);
    xSemaphoreGive(m_tx_mutex);

    return written == static_cast<int>(4 + len + 1);
}

bool BtPlayerUart::sendPlay() {
    ESP_LOGI(TAG, "Sending PLAY command");
    return sendFrame(MsgType::PLAY);
}

bool BtPlayerUart::sendPause() {
    ESP_LOGI(TAG, "Sending PAUSE command");
    return sendFrame(MsgType::PAUSE);
}

bool BtPlayerUart::sendStop() {
    ESP_LOGI(TAG, "Sending STOP command");
    return sendFrame(MsgType::STOP);
}

bool BtPlayerUart::setVolume(uint8_t vol) {
    if (vol > 100) vol = 100;
    ESP_LOGI(TAG, "Sending SET_VOLUME command (%u%%)", vol);
    return sendFrame(MsgType::SET_VOLUME, &vol, 1);
}

bool BtPlayerUart::connectBt(const char* speaker_name) {
    if (speaker_name != nullptr && std::strlen(speaker_name) > 0) {
        size_t n = std::strlen(speaker_name);
        if (n > UART_MAX_PAYLOAD) n = UART_MAX_PAYLOAD;
        ESP_LOGI(TAG, "Sending BT_CONNECT to \"%.*s\"", (int)n, speaker_name);
        return sendFrame(MsgType::BT_CONNECT, reinterpret_cast<const uint8_t*>(speaker_name), n);
    }
    ESP_LOGI(TAG, "Sending BT_CONNECT (default target)");
    return sendFrame(MsgType::BT_CONNECT);
}

bool BtPlayerUart::disconnectBt() {
    ESP_LOGI(TAG, "Sending BT_DISCONNECT");
    return sendFrame(MsgType::BT_DISCONNECT);
}

bool BtPlayerUart::sendPing() {
    return sendFrame(MsgType::PING);
}

void BtPlayerUart::rxTaskTrampoline(void* arg) {
    static_cast<BtPlayerUart*>(arg)->rxTaskLoop();
    vTaskDelete(nullptr);
}

void BtPlayerUart::rxTaskLoop() {
    enum class ParseState { SYNC1, SYNC2, TYPE, LEN, PAYLOAD, CRC };
    ParseState st = ParseState::SYNC1;
    uint8_t type = 0;
    uint8_t len = 0;
    uint8_t idx = 0;
    uint8_t buffer[128];

    while (m_running) {
        int read_len = uart_read_bytes(m_port, buffer, sizeof(buffer), pdMS_TO_TICKS(50));

        // Tick the debounce / stale-link watchdog even when no frame arrived,
        // so time-based transitions (reconnect hold expiry, "no STATUS for 3 s")
        // still fire. ~50 ms cadence when idle.
        evaluateConnectionDebounce();
        static uint32_t total_rx_bytes = 0;
        static int64_t last_rx_log = 0;
        if (read_len > 0) {
            total_rx_bytes += read_len;
            int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms - last_rx_log > 10000) {
                ESP_LOGD(TAG, "UART1 RX alive: %lu bytes total, last chunk %d bytes (0x%02X)",
                         (unsigned long)total_rx_bytes, read_len, buffer[0]);
                last_rx_log = now_ms;
            }
        }

        for (int i = 0; i < read_len; ++i) {
            uint8_t b = buffer[i];
            switch (st) {
                case ParseState::SYNC1:
                    if (b == UART_SYNC_1) {
                        st = ParseState::SYNC2;
                    }
                    break;
                case ParseState::SYNC2:
                    if (b == UART_SYNC_2) {
                        st = ParseState::TYPE;
                    } else if (b != UART_SYNC_1) {
                        st = ParseState::SYNC1;
                    }
                    break;
                case ParseState::TYPE:
                    type = b;
                    st = ParseState::LEN;
                    break;
                case ParseState::LEN:
                    len = b;
                    if (len > UART_MAX_PAYLOAD) {
                        st = ParseState::SYNC1;
                        break;
                    }
                    idx = 0;
                    st = (len > 0) ? ParseState::PAYLOAD : ParseState::CRC;
                    break;
                case ParseState::PAYLOAD:
                    m_rx_payload[idx++] = b;
                    if (idx >= len) {
                        st = ParseState::CRC;
                    }
                    break;
                case ParseState::CRC: {
                    uint8_t crc_check_buf[2 + UART_MAX_PAYLOAD];
                    crc_check_buf[0] = type;
                    crc_check_buf[1] = len;
                    if (len > 0) {
                        std::memcpy(crc_check_buf + 2, m_rx_payload, len);
                    }
                    uint8_t computed = crc8(crc_check_buf, 2 + len);
                    if (computed == b) {
                        handleReceivedFrame(static_cast<MsgType>(type), m_rx_payload, len);
                    } else {
                        ESP_LOGW(TAG, "CRC mismatch on msg 0x%02X: got 0x%02X, expected 0x%02X",
                                 type, b, computed);
                    }
                    st = ParseState::SYNC1;
                    break;
                }
            }
        }
    }
}

void BtPlayerUart::handleReceivedFrame(MsgType type, const uint8_t* payload, size_t len) {
    switch (type) {
        case MsgType::READY: {
            uint16_t fw_version = (len >= 2) ? get_be16(payload) : 0;
            ESP_LOGI(TAG, "Companion READY: fw_version=0x%04X", fw_version);

            EmbeddedSysDb::getInstance().mutate([fw_version](SystemState& s) {
                s.bt_companion.ready = true;
                s.bt_companion.fw_version = fw_version;
            });

            if (m_on_ready) {
                m_on_ready(fw_version);
            }
            break;
        }

        case MsgType::BT_STATUS: {
            bool connected = (len >= 1 && payload[0] != 0);
            std::string speaker_name;
            if (len > 1) {
                speaker_name.assign(reinterpret_cast<const char*>(payload + 1), len - 1);
            }
            ESP_LOGI(TAG, "Companion BT_STATUS: connected=%d, name=\"%s\"",
                     connected, speaker_name.c_str());

            // Feed the debouncer; it owns the SystemState.bt_companion.connected write.
            m_status_link_up = connected;
            if (!speaker_name.empty()) {
                m_last_speaker_name = speaker_name;
            }
            // A disconnect event is authoritative: pin the link down briefly so a
            // stale STATUS(state>=2) arriving in the same batch can't bounce it
            // back up. Reconnect still waits for STATUS state>=2 (no force-up).
            if (!connected) {
                m_force_down_until_us = esp_timer_get_time() + BT_STATUS_DOWN_HOLD_US;
            }
            evaluateConnectionDebounce();
            break;
        }

        case MsgType::STATUS: {
            if (len >= sizeof(StatusPayload)) {
                const auto* p = reinterpret_cast<const StatusPayload*>(payload);
                StatusPayload decoded;
                decoded.state       = p->state;
                decoded.playing     = p->playing;
                decoded.volume      = p->volume;
                decoded.pcm_buf_pct = p->pcm_buf_pct;
                decoded.free_heap   = get_be32(reinterpret_cast<const uint8_t*>(&p->free_heap));
                decoded.underruns   = get_be16(reinterpret_cast<const uint8_t*>(&p->underruns));
                decoded.overruns    = get_be16(reinterpret_cast<const uint8_t*>(&p->overruns));

                // Routine STATUS is debug-only (2 s cadence = console spam). Surface
                // it at INFO only when the drop counters actually *advance* — a bare
                // non-zero total would otherwise re-log every frame forever.
                static uint16_t last_under = 0;
                static uint16_t last_over  = 0;
                ESP_LOGD(TAG, "Companion STATUS: state=%u, playing=%u, buf=%u%%, under=%u, over=%u, heap=%lu",
                         decoded.state, decoded.playing, decoded.pcm_buf_pct, decoded.underruns, decoded.overruns, decoded.free_heap);
                if (decoded.underruns > last_under || decoded.overruns > last_over) {
                    ESP_LOGW(TAG, "Companion drops advanced: under %u->%u, over %u->%u (state=%u buf=%u%% heap=%lu)",
                             last_under, decoded.underruns, last_over, decoded.overruns,
                             decoded.state, decoded.pcm_buf_pct, decoded.free_heap);
                }
                last_under = decoded.underruns;
                last_over  = decoded.overruns;

                // Contract: 0=IDLE, 1=CONNECTING, 2=CONNECTED, 3=PLAYING, 4=ERROR
                bool is_connected = (decoded.state == 2 || decoded.state == 3);

                // Telemetry fields are published immediately; the link-up bit is
                // debounced (see evaluateConnectionDebounce) to keep a flapping
                // A2DP link from machine-gunning the onboard-speaker relay.
                EmbeddedSysDb::getInstance().mutate([&decoded](SystemState& s) {
                    s.bt_companion.ready       = true;
                    s.bt_companion.volume      = decoded.volume;
                    s.bt_companion.pcm_buf_pct = decoded.pcm_buf_pct;
                    s.bt_companion.free_heap   = decoded.free_heap;
                    s.bt_companion.underruns   = decoded.underruns;
                    s.bt_companion.overruns    = decoded.overruns;
                });

                m_status_link_up = is_connected;
                m_last_status_us = esp_timer_get_time();
                evaluateConnectionDebounce();

                if (m_on_status) {
                    m_on_status(decoded);
                }
            }
            break;
        }

        case MsgType::PONG:
            ESP_LOGD(TAG, "Companion PONG received");
            break;

        case MsgType::LOG:
            if (len > 0) {
                std::string log_msg(reinterpret_cast<const char*>(payload), len);
                ESP_LOGI("WROOM", "%s", log_msg.c_str());
            }
            break;

        default:
            ESP_LOGW(TAG, "Unknown message type received: 0x%02X", static_cast<uint8_t>(type));
            break;
    }
}

void BtPlayerUart::evaluateConnectionDebounce() {
    const int64_t now = esp_timer_get_time();

    // Stale-link watchdog: only armed once at least one STATUS frame has arrived,
    // so a slow companion boot doesn't trip it.
    const bool stale = (m_last_status_us != 0) && (now - m_last_status_us > STATUS_STALE_US);

    // Instantaneous target: companion says link up AND telemetry is fresh AND we
    // are not inside a BT_STATUS(disconnect) hold window.
    const bool forced_down = (now < m_force_down_until_us);
    const bool target = m_status_link_up && !stale && !forced_down;
    if (target != m_raw_link_up) {
        m_raw_link_up = target;
        m_raw_link_change_us = now;
    }

    bool publish = false;

    if (!m_link_settled && m_last_status_us != 0) {
        // First authoritative telemetry — publish ground truth immediately so
        // SpeakerPlayback stops assuming "companion active".
        m_debounced_link_up = m_raw_link_up;
        m_link_settled = true;
        publish = true;
    } else if (!m_link_settled && m_init_us != 0 && now - m_init_us > BOOT_GRACE_US) {
        // No STATUS at all by now: companion board absent / UART dead. Settle as
        // disconnected so the onboard speaker takes over instead of staying muted.
        ESP_LOGW(TAG, "No companion STATUS within boot grace — falling back to onboard speaker");
        m_debounced_link_up = false;
        m_link_settled = true;
        publish = true;
    } else if (m_raw_link_up != m_debounced_link_up) {
        // Asymmetric debounce: drop to onboard fast, re-mute onboard slowly.
        const int64_t hold = m_raw_link_up ? RECONNECT_DEBOUNCE_US : DISCONNECT_DEBOUNCE_US;
        if (now - m_raw_link_change_us >= hold) {
            m_debounced_link_up = m_raw_link_up;
            publish = true;
        }
    }

    if (!publish) {
        return;
    }

    const bool up = m_debounced_link_up;
    std::string name = m_last_speaker_name;
    EmbeddedSysDb::getInstance().mutate([up, &name](SystemState& s) {
        s.bt_companion.ready        = true;
        s.bt_companion.connected    = up;
        s.bt_companion.link_settled = true;
        if (!name.empty()) {
            std::strncpy(s.bt_companion.speaker_name, name.c_str(),
                         sizeof(s.bt_companion.speaker_name) - 1);
            s.bt_companion.speaker_name[sizeof(s.bt_companion.speaker_name) - 1] = '\0';
        }
    });

    ESP_LOGI(TAG, "Companion BT link %s (debounced%s) speaker=\"%s\"",
             up ? "UP" : "DOWN",
             stale ? ", stale-watchdog" : "",
             name.c_str());

    if (m_on_bt_status) {
        m_on_bt_status(up, name);
    }
}

} // namespace btplayer
