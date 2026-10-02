#pragma once

#include "common/ReactorTask.h"
#include "WssClient.h"
#include "VoiceAgent.h"
#include "gemini_skills_generated.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include <atomic>
#include "esp_timer.h"
#include <new>
#include <mutex>
#include <string>

// The direct backend: one WebSocket to Gemini Live (VoiceAgent).
class GeminiProtocol : public ReactorTask, public VoiceAgent {
public:
    static GeminiProtocol& getInstance();

    // Model, voice, system prompt and transcripts for the next session. Empty
    // strings mean the firmware defaults. The source is set by the app
    // (system.ndb).
    struct SessionSettings {
        std::string model;
        std::string voice;
        std::string system_prompt;
        bool transcripts = true;      // ask for input and output transcriptions
        bool transcript_log = true;   // print each finished turn to the log
        uint8_t resume_min = 60;      // resume the last conversation if younger; 0 = never
        uint8_t keepalive_s = 60;     // keep the connection after a session; 0 = close
        bool web_search = true;       // add Google Search on models that support it
        // Gemini's voice detection: sensitivity 0 = its default, 1 = low,
        // 2 = high; ms 0 = its default.
        uint8_t vad_start = 1;
        uint8_t vad_end = 0;
        uint16_t vad_prefix_ms = 0;
        uint16_t vad_silence_ms = 0;
    };
    typedef SessionSettings (*SettingsSourceFn)();
    void setSettingsSource(SettingsSourceFn source) { m_settings_source = source; }

    // VoiceAgent
    // Mic audio goes up as realtimeInput (base64 in JSON).
    void sendMicAudio(const uint8_t* pcm, size_t len) override;
    // A realtimeInput text turn.
    void sendTextTurn(const std::string& text) override;
    void sendToolResponse(const char* call_id, const char* json_result) override;
    // The connection is up and Gemini has acknowledged the setup.
    bool ready() override {
        std::lock_guard<std::mutex> lock(m_turn_mutex);
        return m_setup_complete && isConnected();
    }
    // Gives up after REPLY_WAIT_US (the reply can take several seconds over
    // music).
    bool awaitingReply() override;
    // If the reply is still arriving, its remaining audio (and anything
    // Gemini says after it) is queued on the connection, so the session moves
    // to a new connection that resumes the conversation.
    void interruptReply() override;
    void setBargeIn(bool on) override { m_barge_in = on; }
    uint32_t turnsCompleted() const override { return m_turns_completed; }
    // Keeps a healthy, quiet connection open for keepalive_s (mic off, reply
    // audio dropped) so a wake inside that window skips the connect and
    // setup; otherwise closes it.
    void endSession() override;

    bool isConnected() { return m_client.isConnected(); }
    void connect();
    void closeConnection();

    // Test hook (POST /api/assistant/handoff): act as if Gemini had sent
    // goAway. False without a live session.
    bool simulateGoAway();

    // The model that refused Google Search this boot ("models/..."), or "".
    std::string searchRefusedModel() {
        std::lock_guard<std::mutex> lock(m_turn_mutex);
        return m_search_refused_model;
    }
    static constexpr int64_t REPLY_WAIT_US = 15LL * 1000 * 1000;
    // A tool response is retried this many times, waiting this long for the
    // client each time.
    static constexpr int TOOL_SEND_ATTEMPTS = 3;
    static constexpr uint32_t TOOL_SEND_TIMEOUT_MS = 3000;

    // ReactorTask interface
    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

protected:
    void run() override;

private:
    GeminiProtocol();
    ~GeminiProtocol() override;

    bool ensureClientInitialized();
    void transmitSetupHandshake();
    void processIncomingFrame(char* payload, size_t length);
    void handleToolCall(JsonObjectConst toolCall);
    // Input/output transcription fragments → TranscriptLog.
    void recordTranscription(JsonObjectConst serverContent);

    static void websocketEventHandler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
    bool startClientConnection();

    void flushTextTurn();   // m_turn_mutex held
    void resetTextTurn();

    WssClient m_client;
    std::mutex m_client_mutex;
    std::mutex m_turn_mutex;
    std::string m_pending_turn;        // waiting for setupComplete
    bool m_setup_complete = false;
    // False from closeConnection() until the next CONNECTED: reply audio still
    // queued in m_incoming_psram_rb must not restart "speaking" after a close.
    std::atomic<bool> m_accept_audio{false};
    // Guarded by m_turn_mutex.
    int64_t m_reply_wait_us = 0;       // when the owed reply was asked for; 0 = none
    void expectReply();                // start (or restart) the reply wait
    // Session resumption, RAM only. The latest resumable handle Gemini sent
    // and the conversation's last activity (handle, turn end, close); a new
    // connection resumes with it if that is within resume_min. gemini-3.8-live
    // sends one handle per connection, just after setup, and it restores the
    // latest state. Guarded by m_turn_mutex.
    std::string m_resume_handle;
    int64_t m_resume_handle_us = 0;
    bool m_resuming = false;           // this connection's setup carries a handle
    bool m_force_resume = false;       // a handoff resumes whatever resume_min says

    // Restarting the connection inside a session, which the event handler
    // can't do itself: after Gemini refuses an expired handle (close 1008;
    // reconnect fresh at once, not after reconnect_timeout_ms), and for a
    // goAway handoff. ws_state reads CONNECTING meanwhile, so the session
    // carries on.
    std::atomic<bool> m_restart_requested{false};
    std::atomic<bool> m_restart_graceful{false};   // send a close frame first
    std::atomic<bool> m_restarting{false};         // old connection's events are ignored
    static constexpr uint32_t NOTIFY_RESTART_BIT = (1u << 14);
    void requestRestart(bool graceful);
    void restartConnection();

    // goAway: Gemini ends every connection after ~10 min. The session moves
    // to a new connection resuming the same conversation, at the next turn
    // boundary (no reply owed or playing), or at once if the server closes
    // first. One connection at a time: two TLS clients over music don't fit
    // in internal RAM, and the gap between turns is ~1 s.
    std::atomic<bool> m_handoff_pending{false};
    void maybeHandOff(const SystemState& snap);

    std::atomic<bool> m_barge_in{false};
    std::atomic<uint32_t> m_turns_completed{0};
    std::atomic<bool> m_record_transcripts{true};   // the transcripts setting

    // Google Search in the setup. A model without search quota (Gemini 3.x on
    // the free tier) closes the setup with 1011 "exceeded your current
    // quota"; the connection then restarts without search, and that model
    // gets none until the next boot. Guarded by m_turn_mutex.
    bool m_search_in_setup = false;
    bool m_search_quota_closed = false;   // the close frame said "quota" before setupComplete
    std::string m_search_refused_model;
    std::string m_last_setup_model;       // the model the last setup named
    // Set by the websocket handler as soon as an "interrupted" frame arrives,
    // ahead of the reply audio still queued before it: the parser drops that
    // audio instead of waiting for playback to drain. Cleared when the
    // parser reaches the interrupted frame.
    std::atomic<bool> m_interrupt_pending{false};

    // Kept open by endSession() with no session using it.
    std::atomic<bool> m_parked{false};
    esp_timer_handle_t m_park_timer = nullptr;
    static constexpr uint32_t NOTIFY_PARK_EXPIRED_BIT = (1u << 13);
    SettingsSourceFn m_settings_source = nullptr;
    std::string m_ws_uri;

    // Persistent Zero-Allocation Arenas for Audio & Skill Tool execution
    uint8_t* m_static_pcm_scratch_arena = nullptr;
    char* m_static_payload_arena = nullptr;   // mic audio frames
    static constexpr size_t PAYLOAD_ARENA_SIZE = 4096;
    // static constexpr size_t STATIC_PCM_ARENA_MAX_SIZE = 65536; // 64KB max decoded output ceiling

    // Fixed-size memory management variables in PSRAM
    static constexpr size_t PSRAM_RB_SIZE = 512 * 1024;      // 512KB static ring buffer pool
    static constexpr size_t MAX_INCOMING_FRAME_SIZE = 98304; // 96KB max single frame staging space

    // Flow control: how long each stage waits for downstream room before dropping.
    // A single Gemini audio frame is ~1s of speech, so these only expire if playback stalls.
    static constexpr uint32_t VOICE_RX_MAX_BLOCK_MS   = 5000; // parser task -> VOICE_RX_BUF
    static constexpr uint32_t INCOMING_RB_MAX_BLOCK_MS = 5000; // WS task -> m_incoming_psram_rb

    RingbufHandle_t m_incoming_psram_rb = nullptr;
    uint8_t* m_assembly_scratch = nullptr;
    size_t m_assembly_idx = 0;
    bool m_frame_overflowed = false;

    // Session-specific diagnostics statistics
    uint32_t m_rx_frames = 0;
    uint32_t m_rx_dropped_frames = 0;
    uint32_t m_rx_audio_bytes = 0;

    // Per-reply delivery, measured where frames arrive (WS task) and logged at
    // turnComplete: is Gemini's audio arriving faster than real time, and how
    // long was the longest wait between audio frames? Blocked time is the WS
    // task waiting on a full queue (our backpressure, not the network).
    std::atomic<int64_t>  m_reply_first_us{0};
    std::atomic<int64_t>  m_reply_last_us{0};
    std::atomic<int64_t>  m_reply_max_gap_us{0};
    std::atomic<int64_t>  m_reply_blocked_us{0};
    std::atomic<uint32_t> m_reply_frame_bytes{0};
    std::atomic<uint32_t> m_reply_frames{0};
    void noteAudioFrameArrival(size_t bytes);
    void logReplyDelivery(const char* how);

    static constexpr const char* TAG = "GeminiProto";
};


