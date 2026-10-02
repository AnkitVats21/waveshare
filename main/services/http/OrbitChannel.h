#pragma once

#include "media_player/RemoteOutput.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include <ArduinoJson.h>
#include <atomic>
#include <mutex>
#include <string>

namespace Services {

/**
 * @brief Satellites (nexus-orbit on a PC or Pi) at /api/orbit/ws: music can
 * play on one of them instead of the board speaker.
 *
 *   - Up to MAX_SATELLITES at once, apart from the dashboard's /api/ws.
 *     A satellite sends satellite_register, then orbit_report every second;
 *     one silent for TIMEOUT_MS is dropped.
 *   - The board keeps the queue. While media.output_target is SATELLITE,
 *     MusicPlaybackService sends the active satellite orbit_play / pause /
 *     resume / seek / stop through the RemoteOutput interface; the satellite
 *     resolves and plays the track itself. Its reports set media.position_ms
 *     and the playing/paused state; its track_ended advances the queue.
 *   - If the active satellite goes away, the song continues on the board
 *     from the last reported position.
 *
 * No task of its own: frames arrive on the httpd task, sends are queued to
 * it (httpd_queue_work), and a 1 s esp_timer checks the heartbeats.
 */
class OrbitChannel : public RemoteOutput {
public:
    static OrbitChannel& getInstance();

    // Registers with MusicPlaybackService and starts the heartbeat check.
    void begin();

    // Called by HttpService.
    esp_err_t onWsHandshake(httpd_req_t* req);
    esp_err_t handleWsRequest(httpd_req_t* req);
    void onSocketClosed(int sockfd);
    void onServerStopped();

    // {"active_target": id or "local", "satellites": [...]} for /api/ws and
    // GET /api/orbit.
    void toJson(JsonObject out);

    // RemoteOutput
    void play(const InvidiousTrack& track, uint32_t position_ms) override;
    void pause() override;
    void resume() override;
    void seek(uint32_t position_ms) override;
    void stop() override;
    std::vector<Satellite> satellites() override;
    bool select(const std::string& target, std::string& chosen) override;

    static constexpr const char* WS_PATH = "/api/orbit/ws";

private:
    OrbitChannel() = default;
    OrbitChannel(const OrbitChannel&) = delete;
    OrbitChannel& operator=(const OrbitChannel&) = delete;

    struct Slot {
        int fd = -1;
        bool registered = false;
        bool closing = false;      // close requested, waiting for close_fn
        char id[40] = {};
        char name[40] = {};
        char type[16] = {};
        int64_t last_seen_ms = 0;
        // Last report, for the dashboard.
        char state[12] = {};
        uint32_t position_ms = 0;
        int volume = 0;
    };

    void handleFrame(int idx, const char* json, size_t len);
    void handleReport(int idx, JsonDocument& doc);
    void handleEvent(int idx, JsonDocument& doc);
    void handleAction(JsonDocument& doc);
    void sendToActive(const JsonDocument& doc);
    // Caller holds m_mutex.
    int findSlot(int fd) const;
    void sendLocked(int idx, const JsonDocument& doc);
    void closeLocked(int idx, const char* why);
    static void onHeartbeatTimer(void* arg);

    static constexpr int     MAX_SATELLITES  = 3;
    static constexpr int64_t TIMEOUT_MS      = 5000;
    static constexpr size_t  MAX_FRAME_LEN   = 1024;

    std::mutex m_mutex;
    Slot m_slots[MAX_SATELLITES];
    int m_active = -1;  // slot playing the music, or -1
    std::atomic<httpd_handle_t> m_server{nullptr};
    esp_timer_handle_t m_timer = nullptr;

    static constexpr const char* TAG = "Orbit";
};

} // namespace Services
