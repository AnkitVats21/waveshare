#pragma once

#include "common/ReactorTask.h"
#include "WssClient.h"
#include "gemini_skills_generated.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include <atomic>
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
    };
    typedef SessionSettings (*SettingsSourceFn)();
    void setSettingsSource(SettingsSourceFn source) { m_settings_source = source; }

    void transmitToolResponse(const char* call_id, const char* json_result);
    void transmitAudioUplink(const char* base64_pcm);
    
    bool isConnected() { return m_client.isConnected(); }
    void connect();
    void closeConnection();
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
    static constexpr int64_t REPLY_WAIT_US = 15LL * 1000 * 1000;

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
    // and when; a new connection resumes with it (same conversation) if it
    // is younger than resume_min. Guarded by m_turn_mutex.
    std::string m_resume_handle;
    int64_t m_resume_handle_us = 0;
    bool m_resuming = false;           // this connection's setup carries a handle
    // Gemini closes (1008) a setup whose handle has expired. The event
    // handler can't restart the client, so it asks the task to, at once
    // instead of after reconnect_timeout_ms.
    std::atomic<bool> m_retry_fresh{false};
    static constexpr uint32_t NOTIFY_RETRY_BIT = (1u << 14);
    SettingsSourceFn m_settings_source = nullptr;
    std::string m_ws_uri;

    ToolCallHandlerFn m_tool_handler = nullptr;
    void* m_tool_ctx = nullptr;

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


