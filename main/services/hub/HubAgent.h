#pragma once

#include "common/ReactorTask.h"
#include "gemini_live/VoiceAgent.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <atomic>
#include <mutex>
#include <string>

namespace Services {

/**
 * @brief The voice backend through the hub (docs/plans/hub-relay-design.md 4.2).
 *
 * The hub holds the Gemini Live connection; this board sends it the session's
 * setup (GeminiProtocol::buildSessionSetup, so settings, prompt and memory
 * are the same as direct), mic audio as 16 kHz PCM and the board skills'
 * answers, over HubLink. The reply comes down as 24 kHz PCM, paced by the
 * hub, and goes straight into VOICE_RX_BUF from the link's client task.
 *
 * ws_state, while this is the active backend: CONNECTING after session_start,
 * CONNECTED at session_ready, DISCONNECTED at the session's end. A
 * session_error or the link dropping during a session ends it with the
 * error chime (ErrorCooldown); there is no switch to direct mid-session.
 */
class HubAgent : public ReactorTask, public VoiceAgent {
public:
    static HubAgent& getInstance();

    // The voice_backend setting (0 direct, 1 hub, 2 auto), cached for the
    // selector, which runs on the wake word task. Set at boot and by
    // POST /api/hub.
    static void setBackendSetting(uint8_t backend) { s_backend = backend; }
    static uint8_t backendSetting() { return s_backend.load(); }

    // VoiceAgent
    const char* name() const override { return "hub"; }
    void sendMicAudio(const uint8_t* pcm, size_t len) override;
    void sendTextTurn(const std::string& text) override;
    void sendToolResponse(const char* call_id, const char* json_result) override;
    bool ready() override { return m_ready.load(); }
    bool awaitingReply() override;
    void interruptReply() override;
    void setBargeIn(bool on) override { m_barge_in = on; }
    uint32_t turnsCompleted() const override { return m_turns_completed; }
    void endSession() override;

    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

protected:
    void run() override;

private:
    HubAgent();

    void startSession();
    void handleMessage(const std::string& text);
    void failSession(const char* why);
    void replyStarted();
    void replyEnded(const char* how);
    void expectReply();
    bool sendJson(const std::string& text);

    static void onMessage(const std::string& text, void* ctx);
    static void onAudio(uint8_t channel, const uint8_t* data, size_t len, bool start, void* ctx);
    static void onLinkChange(bool up, void* ctx);

    static inline std::atomic<uint8_t> s_backend{0};

    QueueHandle_t m_rx_queue = nullptr;   // std::string* messages from the link task

    std::atomic<bool> m_session{false};       // session_start sent, not ended
    bool m_fail_pending = false;              // hub away at the wake; this task only
    std::atomic<bool> m_ready{false};         // session_ready received
    std::atomic<bool> m_accept_audio{false};  // reply audio goes to the speaker
    std::atomic<bool> m_dropping{false};      // interrupted: drop audio until the turn ends
    std::atomic<bool> m_reply_audio{false};   // this reply's audio has started
    std::atomic<bool> m_barge_in{false};
    std::atomic<uint32_t> m_turns_completed{0};
    std::atomic<bool> m_record_transcripts{true};

    std::mutex m_turn_mutex;
    std::string m_pending_turn;           // sent at session_ready
    int64_t m_reply_wait_us = 0;          // when the owed reply was asked for; 0 = none

    // Mic frames: header + one chunk (MIC_TX_BUF items are up to 1024 bytes).
    uint8_t* m_mic_frame = nullptr;
    static constexpr size_t MIC_FRAME_MAX = 2 + 4096;

    // Per reply, measured where the audio arrives (client task), logged at
    // its end: delivery against real time, the longest gap between frames,
    // and audio dropped because VOICE_RX_BUF was full.
    std::atomic<int64_t> m_reply_first_us{0};
    std::atomic<int64_t> m_reply_last_us{0};
    std::atomic<int64_t> m_reply_max_gap_us{0};
    std::atomic<uint32_t> m_reply_bytes{0};
    std::atomic<uint32_t> m_reply_dropped{0};

    // sysdb notifies reactors with the whole changed mask (field bits in the
    // low 16): these sit above the field bits of ASSISTANT and SYSTEM, as
    // GeminiProtocol's do.
    static constexpr uint32_t BIT_RX            = 1u << 13;
    static constexpr uint32_t BIT_REPLY_STARTED = 1u << 14;
    static constexpr uint32_t BIT_LINK          = 1u << 15;
    // PSRAM. Parsing the ~14 KB setup (nested tool schemas) overflowed 8 KB.
    static constexpr uint32_t STACK_SIZE = 12 * 1024;
    static constexpr int RX_QUEUE_LEN = 16;
    static constexpr int64_t REPLY_WAIT_US = 15LL * 1000 * 1000;
    static constexpr uint32_t VOICE_SEND_WAIT_MS = 20;
    static constexpr uint8_t CHANNEL_MIC = 1;
    static constexpr uint8_t CHANNEL_VOICE = 2;
    static constexpr const char* TAG = "HubAgent";
};

} // namespace Services
