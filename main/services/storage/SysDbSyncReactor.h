#pragma once

#include "common/ReactorTask.h"
#include "common/sysdb/EmbeddedSysDb.h"

namespace Services {

class SysDbSyncReactor : public ReactorTask {
public:
    static SysDbSyncReactor& getInstance();

    bool begin();

    /**
     * @brief Reads persistent state from SD card (/sdcard/state_sync.txt) and restores into SysDb.
     * @return true if state was found and loaded successfully.
     */
    bool loadPersistentState();

    // ReactorTask interface
    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

protected:
    void run() override;

private:
    SysDbSyncReactor();
    ~SysDbSyncReactor() override = default;
    SysDbSyncReactor(const SysDbSyncReactor&) = delete;
    SysDbSyncReactor& operator=(const SysDbSyncReactor&) = delete;

    void writeStateToSD();

    static constexpr const char* TAG = "SysDbSync";
};

} // namespace Services
