#pragma once

#include <atomic>

#include "common/ReactorTask.h"
#include "common/sysdb/EmbeddedSysDb.h"

namespace Services {

// Saves the SystemState fields bound in schema/db/system.star (collection
// "state") to system.ndb, at most SAVE_DELAY_MS after they change, and
// restores them at boot.
class SysDbSyncReactor : public ReactorTask {
public:
    static SysDbSyncReactor& getInstance();

    bool begin();

    // Applies the saved state to SysDb. Call after openSystemDb().
    bool loadPersistentState();

    // While paused, changes are not saved: an OTA update blinks the LED and
    // then reboots, and that blink must not become the saved LED mode.
    void pauseSaving() { m_paused = true; }
    void resumeSaving();

    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

protected:
    void run() override;

private:
    SysDbSyncReactor();
    ~SysDbSyncReactor() override = default;
    SysDbSyncReactor(const SysDbSyncReactor&) = delete;
    SysDbSyncReactor& operator=(const SysDbSyncReactor&) = delete;

    void save();

    static constexpr const char* TAG = "SysDbSync";
    static constexpr uint32_t SAVE_DELAY_MS = 500;
    std::atomic<bool> m_paused{false};
};

} // namespace Services
