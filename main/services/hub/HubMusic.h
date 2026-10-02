#pragma once

#include "common/ReactorTask.h"
#include "media_player/MusicHub.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <string>

namespace Services {

/**
 * @brief The board as the hub's music player and controller (hub phase 3;
 * nexus-orbit docs/PROTOCOL.md section 5).
 *
 * While HubLink::musicAvailable(), MusicPlaybackService sends the person's
 * actions here (sendAction) and its track ends (playerEvent); this task
 * sends them to the hub. From the hub: music_play, pause, resume, seek,
 * stop and music_queue go to MusicPlaybackService's hub* calls;
 * music_state, when the music plays on another edge, is shown in the
 * board's media state (as a satellite), so the dashboard and keys follow it.
 * Up the link: music_report on change and every second while playing,
 * voice_activity for the board's sessions, and music_sync when the link
 * comes up (the board's song and queue become the hub's).
 *
 * Link messages and outgoing frames go through this task's queues: the
 * callers (the link, the player, keys, httpd) never wait on the network.
 */
class HubMusic : public ReactorTask, public MusicHub {
public:
    static HubMusic& getInstance();

    // MusicHub
    bool active() const override;
    bool sendAction(const Action& action) override;
    void playerEvent(uint32_t seq, const char* id, const char* event, const char* reason) override;

    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

protected:
    void run() override;

private:
    HubMusic();

    void handleMessage(const std::string& text);
    void linkChanged();
    void sendSync(const SystemState& snap);
    void report(const SystemState& snap, bool force);
    void mirror(const char* output, const char* state, const char* id, const char* title,
                const char* artist, uint32_t pos_ms, uint32_t dur_ms);
    void unmirror();
    bool queueTx(std::string* msg);

    static void onMessage(const std::string& text, void* ctx);
    static void onLinkChange(bool up, void* ctx);

    QueueHandle_t m_rx_queue = nullptr;   // std::string* from the link
    QueueHandle_t m_tx_queue = nullptr;   // std::string* to the hub
    std::string m_id;                     // this board's edge id (as in the hello)

    // This task only.
    bool m_mirroring = false;             // the music plays on another edge
    bool m_session = false;               // a voice session is on (sent as voice_activity)
    std::string m_last_state;             // the last report's state and id
    std::string m_last_id;
    int64_t m_last_report_us = 0;

    // sysdb notifies with field bits in the low 16 and components above;
    // these are unused component bits.
    static constexpr uint32_t BIT_RX   = 1u << 30;
    static constexpr uint32_t BIT_TX   = 1u << 29;
    static constexpr uint32_t BIT_LINK = 1u << 28;
    static constexpr uint32_t STACK_SIZE = 6 * 1024;   // PSRAM
    static constexpr int QUEUE_LEN = 16;
    static constexpr int64_t REPORT_EVERY_US = 1000 * 1000;
    static constexpr size_t QUEUE_SYNC_MAX = 20;
    static constexpr const char* TAG = "HubMusic";
};

} // namespace Services
