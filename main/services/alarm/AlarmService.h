#pragma once

#include "common/ReactorTask.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include <vector>
#include <string>
#include <mutex>

namespace Services {

struct Alarm {
    int id;
    int hour;      // 0-23
    int minute;    // 0-59
    char tone_file[64]; // e.g. "/sdcard/alarms/chime.wav"
    bool enabled;
};

class AlarmService : public ReactorTask {
public:
    static AlarmService& getInstance();
    
    bool begin();
    
    void loadAlarms();
    void saveAlarms();
    void addOrUpdateAlarm(const Alarm& alarm);
    void deleteAlarm(int id);
    std::vector<Alarm> getAlarms();
    
    void triggerAlarm(const Alarm& alarm);
    void stopActiveAlarm();

    // ReactorTask interface
    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

protected:
    void run() override;

private:
    AlarmService();
    ~AlarmService() override;
    AlarmService(const AlarmService&) = delete;
    AlarmService& operator=(const AlarmService&) = delete;

    std::vector<Alarm> m_alarms;
    std::mutex m_alarms_mutex;
    
    bool m_playing_alarm = false;
    char m_active_tone_file[64] = {};
    
    static constexpr const char* TAG = "AlarmSvc";
};

} // namespace Services
