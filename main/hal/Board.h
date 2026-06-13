#pragma once

#include "esp_err.h"
#include "esp_io_expander.h"
#include "hal/Board_defs.h"
#include "hal/audio/AudioHal.h"
#include "hal/io/I2CBus.h"
#include "hal/io/IoExpander.h"
#include "hal/led/LedStripManager.h"
#include "hal/storage/SdCardManager.h"
#include <cstdint>

/**
 * @brief Hardware orchestration layer and Dependency Injection factory.
 *
 * Board owns all concrete HAL component instances, coordinates their
 * initialization in the correct hardware-dependent order, and exposes
 * typed references for Dependency Injection into application services.
 *
 * ## What Board DOES
 *  - Initializes hardware in the correct dependency order
 *  - Exposes typed sub-HAL getters (getAudio(), getLeds(), etc.)
 *  - Owns the unique AudioHal, LedStripManager, IoExpander instances
 *
 * ## What Board does NOT do
 *  - Implement IAudioFeedSource or HalBase (removed — AudioHal does that now)
 *  - Wrap every sub-HAL method as a pass-through (removed — callers use getAudio().method())
 *  - Touch EventBus or SystemContext
 */
class Board {
public:
    static Board& getInstance();

    /**
     * @brief Initialize all board peripherals in dependency order.
     * @return true on success.
     */
    bool begin();

    /** @return true if begin() has completed successfully. */
    bool isInitialized() const { return m_initialized; }

    // ── Pre-init configuration (call before begin()) ─────────────────────────
    void setSampleRate(uint32_t sample_rate)     { m_sample_rate    = sample_rate; }
    void setInitialVolumes(int record, int play)  { m_record_volume  = record; m_play_volume = play; }

    // ── Typed sub-HAL getters (Dependency Injection) ─────────────────────────
    /** @brief Direct reference to the AudioHal driver (inject into AudioService / WakeWordEngine). */
    AudioHal&        getAudio()               { return m_audio; }

    /** @brief Direct reference to the LED strip driver (inject into LedService). */
    LedStripManager& getLeds()                { return m_leds; }

    /** @brief Direct reference to the I/O expander (inject into KeyService). */
    IoExpander&      getIoExpanderInstance()  { return m_io; }

    /** @brief Direct reference to SD card storage (inject into storage consumers). */
    SdCardManager&   getStorage()             { return m_storage; }

    // ── Raw handle accessors (kept for HardwareAudioHandles population) ───────
    i2c_master_bus_handle_t  getI2cBus()    { return m_i2c.getBusHandle(); }
    i2s_chan_handle_t        getTxHandle()  { return m_audio.getTxHandle(); }
    i2s_chan_handle_t        getRxHandle()  { return m_audio.getRxHandle(); }
    esp_codec_dev_handle_t   getPlayDev()  { return m_audio.getPlayDev(); }
    esp_codec_dev_handle_t   getRecordDev(){ return m_audio.getRecordDev(); }

    // ── Audio policy helpers (Board-level guards remain here) ─────────────────
    esp_err_t reinitAudio(uint32_t sample_rate);
    esp_err_t setHardwareSampleRate(uint32_t sample_rate);
    esp_err_t setRecordGain(float db_value, bool force = false);
    void      setPreviousVolume() { m_audio.setPreviousVolume(); }

    // ── Storage ───────────────────────────────────────────────────────────────
    esp_err_t initSdCard(const char* mount_point, size_t max_files) {
        return m_storage.mount(mount_point, max_files);
    }

private:
    Board()  = default;
    ~Board() = default;
    Board(const Board&)            = delete;
    Board& operator=(const Board&) = delete;

    // HAL component instances — Board is the sole owner
    I2CBus          m_i2c;
    IoExpander      m_io;
    AudioHal        m_audio;
    LedStripManager m_leds;
    SdCardManager   m_storage;

    // Pre-init settings (forwarded to AudioHal::Config on begin())
    uint32_t m_sample_rate    = 16000;
    int      m_record_volume  = 70;
    int      m_play_volume    = 80;
    float    m_current_mic_gain = -999.0f;
    bool     m_initialized    = false;

    static constexpr const char* TAG = "Board";
};
