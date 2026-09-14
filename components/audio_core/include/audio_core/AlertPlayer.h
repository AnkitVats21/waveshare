#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <cstdint>

enum AlertType {
    ALERT_WAKE_CONFIRM,
    ALERT_READY_TO_SPEAK,
    ALERT_SESSION_END,
    ALERT_ERROR,
    ALERT_OFFLINE
};

class IAlertFileDecoder {
public:
    virtual ~IAlertFileDecoder() = default;
    virtual bool playAlertFile(const char* path) = 0;
};

/**
 * @brief Dedicated subsystem for audio alert chimes and system notifications.
 *
 * Runs an isolated worker task that decodes alert files or synthesizes tones directly
 * to the ALERT_RX_BUF. Completely decoupled from NexusPlayer and AudioEngine to prevent
 * decoder clobbering and race conditions.
 */
class AlertPlayer {
public:
    static AlertPlayer& getInstance();

    bool begin();
    bool start();
    void stop();

    void setFileDecoder(IAlertFileDecoder* decoder) { m_file_decoder = decoder; }

    /**
     * @brief Asynchronously enqueues an alert to be played.
     *        Thread-safe, non-blocking, safe to call from any task.
     */
    void playAlert(AlertType type);

private:
    AlertPlayer();
    ~AlertPlayer();
    AlertPlayer(const AlertPlayer&) = delete;
    AlertPlayer& operator=(const AlertPlayer&) = delete;

    static void workerTaskThunk(void* pvParameters);
    void workerTask();

    void processAlert(AlertType type);
    void playTone(float freq_hz, int16_t volume, uint32_t duration_ms, uint32_t fade_ms);

    IAlertFileDecoder* m_file_decoder = nullptr;
    QueueHandle_t m_queue = nullptr;
    TaskHandle_t  m_task_handle = nullptr;
    volatile bool m_running = false;

    static constexpr const char* TAG = "AlertPlayer";
};
