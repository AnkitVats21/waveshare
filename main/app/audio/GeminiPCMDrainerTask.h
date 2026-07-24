#pragma once

#include "common/TaskBase.h"
#include "services/BufferManager.h"

// ── GEMINI_PCM_BUF ────────────────────────────────────────────────────────────
// 512 KB PSRAM ring buffer that absorbs the burst of decoded PCM from Gemini
// WebSocket frames.  The drainer task paces data to SPK_RX_BUF at the speaker's
// consume rate, forming a proper backpressure chain all the way to the TCP stack.
DECLARE_BUFFER(GEMINI_PCM_BUF, "gemini_pcm", 1024 * 1024)

/**
 * @brief Paces Gemini PCM audio from GEMINI_PCM_BUF → SPK_RX_BUF.
 *
 * GeminiProtocol::processIncomingFrame() writes decoded PCM into GEMINI_PCM_BUF
 * using portMAX_DELAY (blocking = backpressure into the TCP receive window).
 * This task drains it in 20 ms chunks and hands them to SpeakerPlaybackTask
 * through SPK_RX_BUF with a short send timeout for back-pressure.
 *
 * Lifecycle:
 *   - Call suspend() before playing alert tones so alert PCM and Gemini PCM
 *     do not interleave in SPK_RX_BUF.
 *   - Call resume() (+ flush GEMINI_PCM_BUF) after alerts finish.
 *   - Call suspendAndFlush() as a combined convenience for both steps.
 */
class GeminiPCMDrainerTask : public TaskBase {
public:
    static GeminiPCMDrainerTask& getInstance();

    // Prevent task from forwarding Gemini PCM to SPK_RX_BUF.
    // Does NOT flush GEMINI_PCM_BUF so any incoming frames keep accumulating.
    void suspend();

    // Allow forwarding again.
    void resume();

    // Suspend + flush GEMINI_PCM_BUF atomically (used before alert playback).
    void suspendAndFlush();

protected:
    void run() override;

private:
    GeminiPCMDrainerTask();
    ~GeminiPCMDrainerTask() override = default;

    volatile bool m_suspended = false;

    // 20 ms worth of 32 kHz mono int16 PCM = 640 bytes.
    // Matches SpeakerPlaybackTask::TARGET_FRAME_MS for smooth hand-off.
    static constexpr size_t   DRAIN_CHUNK_BYTES      = 640;
    static constexpr uint32_t DRAIN_SEND_TIMEOUT_MS  = 20;
    static constexpr uint32_t IDLE_POLL_MS           = 50;

    static constexpr const char* TAG = "GeminiDrainer";
};
