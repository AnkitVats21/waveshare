#pragma once

#include "common/ReactorTask.h"
#include "WssClient.h"
#include "gemini_skills_generated.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include <atomic>
#include "esp_timer.h"
#include <new>
#include <mutex>
#include <string>

class GeminiProtocol : public ReactorTask {
public:
    static GeminiProtocol& getInstance();

    typedef void (*ToolCallHandlerFn)(const GeminiSkills::DecodedSkillCall& skill_call, void* ctx);

    void setToolCallHandler(ToolCallHandlerFn handler, void* ctx) {
        m_tool_handler = handler;
        m_tool_ctx = ctx;
    }

    // Remote tool call handler (e.g. MCP tools). Returns true if the call was accepted/dispatched.
    typedef bool (*RemoteToolCallHandlerFn)(const char* call_id, const char* name, JsonObjectConst args, void* ctx);
    void setRemoteToolCallHandler(RemoteToolCallHandlerFn handler, void* ctx) {
        m_remote_tool_handler = handler;
        m_remote_tool_ctx = ctx;
    }

    // Remote function declarations hook (e.g. MCP tools to add to Gemini setup handshake).
    typedef void (*RemoteToolsDeclarationsFn)(JsonArray& functionDeclarations, void* ctx);
    void setRemoteToolsDeclarationsSource(RemoteToolsDeclarationsFn source, void* ctx) {
        m_remote_decls_source = source;
        m_remote_decls_ctx = ctx;
    }

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
        bool web_search = true;       // add Google Search to the tools
        // Gemini's voice detection: sensitivity 0 = its default, 1 = low,
        // 2 = high; ms 0 = its default.
        uint8_t vad_start = 1;
        uint8_t vad_end = 0;
        uint16_t vad_prefix_ms = 0;
        uint16_t vad_silence_ms = 0;
    };
    typedef SessionSettings (*SettingsSourceFn)();
    void setSettingsSource(SettingsSourceFn source) { m_settings_source = source; }

    void transmitToolResponse(const char* call_id, const char* json_result);
    void transmitAudioUplink(const char* base64_pcm);
    
    bool isConnected() { return m_client.isConnected(); }
    void connect();
    void closeConnection();
    // The session is over. Keeps a healthy, quiet connection open for
    // keepalive_s (mic off, reply audio dropped) so a wake inside that window
    // skips the connect and setup; otherwise closes it.
    void endSession();
    void forceReconnect() { connect(); }
    void sendTextDirect(const char* text);

    // Sends `text` as a user turn (realtimeInput text) once the session is set
    // up: now if it already is, else when setupComplete arrives. Dropped if the
    // connection closes first. Used by reminders to have Gemini speak.
    void sendTextTurn(const std::string& text);
    // A reply is owed and hasn't started: a text turn was sent (or queued),
    // the person's speech was transcribed, or a tool call was answered. The
    // VAD silence timeout must not end the session meanwhile (the reply can
    // take several seconds over music). Gives up after REPLY_WAIT_US.
    bool awaitingReply();
    // The connection is up and Gemini has acknowledged the setup.
    bool setupComplete() {
        std::lock_guard<std::mutex> lock(m_turn_mutex);
        return m_setup_complete && isConnected();
    }

    // Test hook (POST /api/assistant/handoff): act as if Gemini had sent
    // goAway. False without a live session.
    bool simulateGoAway();

    // Barge-in: mic audio goes up while a reply plays, and an "interrupted"
    // from Gemini drops the rest of the reply.
    void setBargeIn(bool on) { m_barge_in = on; }
    // Model turns finished since boot; a tool can tell whether the model
    // spoke (and the user could answer) between two of its calls.
    uint32_t turnsCompleted() const { return m_turns_completed; }
    // The model that refused Google Search this boot ("models/..."), or "".
    std::string searchRefusedModel() {
        std::lock_guard<std::mutex> lock(m_turn_mutex);
        return m_search_refused_model;
    }
    // Stops the reply now (the person talked over it). If the reply is
    // still arriving, its remaining audio (and anything Gemini says after
    // it) is queued on the connection, so the session moves to a new
    // connection that resumes the conversation.
    void interruptReply();
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

    ToolCallHandlerFn m_tool_handler = nullptr;
    void* m_tool_ctx = nullptr;
    RemoteToolCallHandlerFn m_remote_tool_handler = nullptr;
    void* m_remote_tool_ctx = nullptr;
    RemoteToolsDeclarationsFn m_remote_decls_source = nullptr;
    void* m_remote_decls_ctx = nullptr;

    // Persistent Zero-Allocation Arenas for Audio & Skill Tool execution
    uint8_t* m_static_pcm_scratch_arena = nullptr;
    char* m_static_payload_arena = nullptr;
    // static constexpr size_t STATIC_PCM_ARENA_MAX_SIZE = 65536; // 64KB max decoded output ceiling
    GeminiSkills::DecodedSkillCall m_static_skill_event_slot;

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

    static constexpr const char* TAG = "GeminiProto";
};


