#pragma once

#include "common/ReactorTask.h"
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

    static constexpr const char *TAG = "KeySvc";
};