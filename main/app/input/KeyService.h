#pragma once

#include "core_sysdb/ReactorTask.h"
#include "hal/input/ExpanderKeyInput.h"

class KeyService : public ReactorTask {
public:
    explicit KeyService(ExpanderKeyInput &input);
    ~KeyService() override = default;

    bool begin();

    // ReactorTask interface
    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

protected:
    void run() override;

private:
    ExpanderKeyInput &m_input;
    bool m_prevState[5] = {false};
    uint32_t m_pressCount[5] = {0};
    bool m_longPressedTriggered[5] = {false};
    bool m_key2StopConsumedThisPress = false;
    // KEY_2 held SLEEP_HOLD_MS: deep sleep on release.
    bool m_key2Sleep = false;

    // KEY_2 still down after `ticks` polls: the LEDs fill, then sleep is armed.
    void key2Hold(uint32_t ticks);
    // Press taken by a ringing or snoozed alarm: no normal key action for it.
    bool m_alarmPress[5] = {false};

    static constexpr const char *TAG = "KeySvc";
};