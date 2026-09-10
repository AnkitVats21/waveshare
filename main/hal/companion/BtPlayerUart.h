#pragma once

#include "BtPlayerProtocol.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <functional>
#include <string>

namespace btplayer {

class BtPlayerUart {
public:
    struct Config {
        int uart_num       = 1;
        int tx_pin         = 7;
        int rx_pin         = 8;
        int baudrate       = 460800;
        int rx_buffer_size = 2048;
        int tx_buffer_size = 1024;
    };

    using ReadyCallback    = std::function<void(uint16_t fw_version)>;
    using BtStatusCallback = std::function<void(bool connected, const std::string& name)>;
    using StatusCallback   = std::function<void(const StatusPayload& status)>;

    static BtPlayerUart& getInstance();

    bool init(const Config& config);
    void deinit();

    // Command transmission
    bool sendPlay();
    bool sendPause();
    bool sendStop();
    bool setVolume(uint8_t vol);
    bool connectBt(const char* speaker_name = nullptr);
    bool disconnectBt();
    bool sendPing();

    // Generic frame transmission
    bool sendFrame(MsgType type, const uint8_t* payload = nullptr, size_t len = 0);

    // Callbacks
    void setOnReady(ReadyCallback cb) { m_on_ready = std::move(cb); }
    void setOnBtStatus(BtStatusCallback cb) { m_on_bt_status = std::move(cb); }
    void setOnStatus(StatusCallback cb) { m_on_status = std::move(cb); }

    bool isInitialized() const { return m_running; }

private:
    BtPlayerUart() = default;
    ~BtPlayerUart();
    BtPlayerUart(const BtPlayerUart&) = delete;
    BtPlayerUart& operator=(const BtPlayerUart&) = delete;

    static void rxTaskTrampoline(void* arg);
    void rxTaskLoop();
    void handleReceivedFrame(MsgType type, const uint8_t* payload, size_t len);

    // Connection-state debounce + stale-link watchdog. Called on every STATUS /
    // BT_STATUS frame and on every RX-loop tick (~50 ms) so time-based
    // transitions still fire when no new frame arrives.
    void evaluateConnectionDebounce();

    // Debounce tuning (microseconds).
    static constexpr int64_t DISCONNECT_DEBOUNCE_US = 120'000;   // fast drop to onboard
    static constexpr int64_t RECONNECT_DEBOUNCE_US  = 400'000;   // slow re-mute of onboard
    static constexpr int64_t STATUS_STALE_US        = 3'000'000; // no STATUS => treat as down
    static constexpr int64_t BOOT_GRACE_US          = 8'000'000; // no STATUS ever by now => companion absent, use onboard
    static constexpr int64_t BT_STATUS_DOWN_HOLD_US = 1'500'000; // BT_STATUS(down) overrides a trailing stale STATUS for this long

    uart_port_t        m_port{UART_NUM_1};
    TaskHandle_t       m_rx_task{nullptr};
    SemaphoreHandle_t  m_tx_mutex{nullptr};
    volatile bool      m_running{false};

    uint8_t            m_rx_payload[UART_MAX_PAYLOAD]{0};

    ReadyCallback      m_on_ready;
    BtStatusCallback   m_on_bt_status;
    StatusCallback     m_on_status;

    // Debounce bookkeeping (rx-task context only).
    bool     m_raw_link_up{false};        // instantaneous target from last frame + staleness
    bool     m_debounced_link_up{false};  // value published to SystemState.bt_companion.connected
    bool     m_link_settled{false};       // true once first authoritative publish has happened
    int64_t  m_raw_link_change_us{0};     // when m_raw_link_up last flipped
    int64_t  m_init_us{0};                // when the RX task started (boot-grace anchor)
    int64_t  m_last_status_us{0};         // 0 => no STATUS ever seen (watchdog disarmed)
    int64_t  m_force_down_until_us{0};    // BT_STATUS(disconnect) pins link down past a trailing stale STATUS
    bool     m_status_link_up{false};     // last raw (state==2||3) from a STATUS frame
    std::string m_last_speaker_name;

    static constexpr const char* TAG = "BtPlayerUart";
};

} // namespace btplayer
