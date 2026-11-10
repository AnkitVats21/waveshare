#include "app/input/KeyService.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "core_sysdb/EmbeddedSysDb.h"
#include "core_sysdb/thread_config.h"
#include "media_player/MusicPlaybackService.h"
#include "app/audio/recording/AudioRecorder.h"
#include "freertos/idf_additions.h"
#include <algorithm>

static auto& sysdb = EmbeddedSysDb::getInstance();

static constexpr uint32_t POLL_MS = 20;
static constexpr int VOLUME_STEP = 5;

//                                    down                         click                                   long press
const KeyService::Binding KeyService::BINDINGS[KEY_COUNT] = {
    {KeyId::KEY_2, &KeyService::stopRecording, &KeyService::startProcessedRecording, &KeyService::startStereoRecording},
    {KeyId::KEY_3, nullptr,                    &KeyService::volumeUp,                &KeyService::nextTrack},
    {KeyId::KEY_4, nullptr,                    &KeyService::togglePlay,              nullptr},
    {KeyId::KEY_5, nullptr,                    &KeyService::volumeDown,              &KeyService::previousTrack},
};

KeyService::KeyService(ExpanderKeyInput& input, MusicPlaybackService& music, AudioRecorder& recorder)
    : ReactorTask({
          "key_svc",
          ThreadConfig::StackSize::STACK_MEDIUM,
          ThreadConfig::Priority::KEY_POLL,
          ThreadConfig::CORE_NETWORK,
          0 // Pure writer, does not watch any state components
      })
    , m_input(input)
    , m_music(music)
    , m_recorder(recorder)
{}

void KeyService::run() {
    ESP_LOGI(TAG, "KeyService polling loop active.");
    while (m_running) {
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        for (size_t i = 0; i < KEY_COUNT; ++i) {
            const Event e = m_keys[i].gesture.update(m_input.isPressed(BINDINGS[i].key), now_ms);
            if (e != Event::None) handle(BINDINGS[i], m_keys[i], e);
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void KeyService::handle(const Binding& b, Key& k, Event e) {
    const int n = (int)b.key + 1;
    if (e == Event::Down) {
        ESP_LOGI(TAG, "Key %d pressed", n);
        k.taken = alarmTakes(b.key) || (b.down && (this->*b.down)());
        return;
    }
    if (k.taken) return;
    if (e == Event::Click && b.click) (this->*b.click)();
    if (e == Event::LongPress && b.longPress) (this->*b.longPress)();
}

// While an alarm rings or is snoozed, the keys belong to it: key 2 stops it,
// any other key snoozes it.
bool KeyService::alarmTakes(KeyId key) {
    if (sysdb.snapshot().alarm.state == AlarmRingState::IDLE) return false;
    const bool stop = key == KeyId::KEY_2;
    ESP_LOGI(TAG, "Key %d: %s the alarm", (int)key + 1, stop ? "stopping" : "snoozing");
    sysdb.mutate([stop](SystemState& s) {
        if (stop) s.alarm.stop_requested = true;
        else s.alarm.snooze_requested = true;
    });
    return true;
}

// ── Actions ─────────────────────────────────────────────────────────────────

bool KeyService::stopRecording() {
    if (!m_recorder.isRecording()) return false;
    ESP_LOGI(TAG, "Stopping the recording");
    m_recorder.stopRecording(AudioRecorder::StopReason::MANUAL);
    return true;
}

void KeyService::startProcessedRecording() {
    ESP_LOGI(TAG, "Starting a PROCESSED recording");
    m_recorder.startRecording(AudioRecorder::RecordMode::PROCESSED);
}

void KeyService::startStereoRecording() {
    ESP_LOGI(TAG, "Starting a STEREO recording");
    m_recorder.startRecording(AudioRecorder::RecordMode::STEREO);
}

void KeyService::volumeUp() { stepVolume(VOLUME_STEP); }
void KeyService::volumeDown() { stepVolume(-VOLUME_STEP); }

void KeyService::stepVolume(int delta) {
    sysdb.mutate([delta](SystemState& s) {
        const int old_vol = s.audio.speaker_volume;
        s.audio.speaker_volume = std::clamp(old_vol + delta, 0, 100);
        ESP_LOGI(TAG, "Volume %d -> %d", old_vol, s.audio.speaker_volume);
    });
}

void KeyService::togglePlay() {
    ESP_LOGI(TAG, "Toggling playback");
    m_music.postCommand(MediaCmdType::TOGGLE_PLAY_PAUSE);
}

void KeyService::nextTrack() {
    ESP_LOGI(TAG, "Next track");
    m_music.postCommand(MediaCmdType::NEXT);
}

void KeyService::previousTrack() {
    ESP_LOGI(TAG, "Previous track");
    m_music.postCommand(MediaCmdType::PREVIOUS);
}
