#pragma once

/**
 * @brief Abstract control interface for companion transport commands (play/pause).
 * Decouples audio_core from the hal/companion UART driver.
 */
class ICompanionControl {
public:
    virtual ~ICompanionControl() = default;
    virtual bool isInitialized() const = 0;
    virtual bool sendPlay() = 0;
    virtual bool sendPause() = 0;
};
