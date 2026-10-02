#pragma once

#include "common/ReactorTask.h"
#include "esp_http_server.h"
#include <atomic>
#include <string>

namespace Services {

/**
 * @brief Live control channel for clients: WebSocket at /api/ws on the HTTP server.
 *
 *   - Exactly one client at a time: a new connection takes over and the old
 *     socket is closed (personal device, no multi-client bookkeeping).
 *   - Device -> client: full JSON state on connect, then again on every relevant
 *     SysDb change (coalesced to MIN_PUSH_INTERVAL_MS) and every TELEMETRY_PERIOD_MS.
 *   - Client -> device: {"cmd": "<name>", ...} command envelope.
 *   - Transcript (opt-in, {"cmd":"subscribe","transcript":true}): the whole
 *     transcript once, then {"type":"transcript","seq","entries"} with the
 *     entries that changed (see TranscriptLog). State pushes carry no "type".
 *   - Advertises the device over mDNS as nexus.local (_nexus._tcp, _http._tcp).
 *
 * A relay (the hub link, HubLink) gets the same stream: every state push and
 * every transcript change (always subscribed), and its {"cmd"} frames are
 * handled like the client's. The hub fans them out to its own browsers, so
 * the device keeps one protocol however a client reaches it.
 */
class ControlChannel : public ReactorTask {
public:
    static ControlChannel& getInstance();

    bool begin();

    // Called by HttpService.
    esp_err_t onWsHandshake(httpd_req_t* req);   // ws_post_handshake_cb
    esp_err_t handleWsRequest(httpd_req_t* req); // data frames
    void onSocketClosed(int sockfd);
    void onServerStopped();

    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

    // The relay. send() returns false if the frame didn't go out; it is
    // called on this channel's task, and must not block for long.
    typedef bool (*RelaySendFn)(const std::string& text, void* ctx);
    void setRelay(RelaySendFn send, void* ctx) { m_relay_send = send; m_relay_ctx = ctx; }
    // The relay is up (full state and transcript follow) or down.
    void setRelayUp(bool up);
    // A {"cmd"} frame from the relay.
    void handleRelayCommand(const char* json, size_t len);

protected:
    void run() override;

private:
    ControlChannel();
    ControlChannel(const ControlChannel&) = delete;
    ControlChannel& operator=(const ControlChannel&) = delete;

    void startMdns();
    bool hasListener() const { return m_client_fd.load() >= 0 || m_relay_up.load(); }
    void pushState();
    void buildState(std::string& out);
    void pushTranscript();
    // The transcript entries newer than *sent_seq as JSON ("" if none); moves
    // *sent_seq on.
    bool buildTranscript(uint32_t& sent_seq, std::string& out);
    void sendText(httpd_handle_t server, int fd, std::string& text);
    void sendRelay(const std::string& text);
    static void onTranscriptChanged(void* ctx);
    void handleCommand(const char* json, size_t len, bool from_relay);
    void wake();

    std::atomic<httpd_handle_t> m_server{nullptr};
    std::atomic<int>            m_client_fd{-1};
    std::atomic<bool>           m_push_now{false};
    std::atomic<bool>           m_transcript_subscribed{false};
    std::atomic<bool>           m_transcript_dirty{false};
    std::atomic<bool>           m_transcript_resend{false};  // next push starts from seq 0
    uint32_t                    m_transcript_sent_seq = 0;    // run() task only

    RelaySendFn                 m_relay_send = nullptr;
    void*                       m_relay_ctx = nullptr;
    std::atomic<bool>           m_relay_up{false};
    std::atomic<bool>           m_relay_resend{false};        // next relay push starts from seq 0
    uint32_t                    m_relay_sent_seq = 0;         // run() task only

    static constexpr uint32_t MIN_PUSH_INTERVAL_MS = 200;
    static constexpr uint32_t TELEMETRY_PERIOD_MS  = 1000;
    static constexpr size_t   MAX_COMMAND_LEN      = 2048;

    static constexpr const char* MDNS_HOSTNAME = "nexus";
    static constexpr const char* TAG = "ControlCh";
};

} // namespace Services
