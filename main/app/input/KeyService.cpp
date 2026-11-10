#include "app/input/KeyService.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "core_sysdb/EmbeddedSysDb.h"
#include "core_sysdb/thread_config.h"
#include "media_player/MusicPlaybackService.h"
#include "app/audio/recording/AudioRecorder.h"
#include "app/led/LedService.h"
#include "freertos/idf_additions.h"
#include <algorithm>

static auto& sysdb = EmbeddedSysDb::getInstance();

static constexpr uint32_t POLL_MS = 20;
static constexpr int VOLUME_STEP = 5;

//                                    down                         click                                   long press
const KeyService::Binding KeyService::BINDINGS[KEY_COUNT] = {
    {KeyId::KEY_2, &KeyService::stopRecording, &KeyService::startProcessedRecording, &KeyService::startStereoRecording},
    {KeyId::KEY_3, nullptr,                    &KeyService::volumeUp,                &KeyService::nextTrack},
    {KeyId::KEY_4, nullptr,                    &KeyService::togglePlay,              &KeyService::nextVolumeTarget},
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

// The hub speaker with this id, from the hub's last music_state; nullptr if
// it isn't there (or the hub doesn't run the music).
static const MusicHub::Speaker* findSpeaker(const MusicHub::Speakers& sp, const std::string& id) {
    for (const MusicHub::Speaker& s : sp.list) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

void KeyService::stepVolume(int delta) {
    if (!m_volume_target.empty()) {
        const MusicHub::Speakers sp = m_music.hubActive() ? m_music.hubSpeakers() : MusicHub::Speakers{};
        const MusicHub::Speaker* s = findSpeaker(sp, m_volume_target);
        if (s && m_music.hubVolume(s->id.c_str(), delta)) {
            ESP_LOGI(TAG, "Volume of %s %+d", s->name.c_str(), delta);
            // The level it will report back (unknown: the step alone).
            if (s->volume >= 0) LedService::showLevel(s->volume + delta, BLUE_LED);
            return;
        }
        ESP_LOGI(TAG, "Speaker %s gone: the volume keys are the board's again", m_volume_target.c_str());
        m_volume_target.clear();
    }
    int level = 0;
    sysdb.mutate([delta, &level](SystemState& s) {
        const int old_vol = s.audio.speaker_volume;
        s.audio.speaker_volume = std::clamp(old_vol + delta, 0, 100);
        level = s.audio.speaker_volume;
        ESP_LOGI(TAG, "Volume %d -> %d", old_vol, level);
    });
    LedService::showLevel(level, WHITE_LED);
}

// Board, then each hub speaker in turn, then the board again. With no
// speakers it stays on the board (the ring shows the board's level).
void KeyService::nextVolumeTarget() {
    const MusicHub::Speakers sp = m_music.hubActive() ? m_music.hubSpeakers() : MusicHub::Speakers{};
    size_t at = 0;   // 0: the board; i + 1: sp.list[i]
    for (size_t i = 0; i < sp.list.size(); ++i) {
        if (sp.list[i].id == m_volume_target) at = i + 1;
    }
    at = (at + 1) % (sp.list.size() + 1);
    if (at == 0) {
        m_volume_target.clear();
        ESP_LOGI(TAG, "Volume keys: the board");
        LedService::showLevel(sysdb.snapshot().audio.speaker_volume, WHITE_LED);
        return;
    }
    const MusicHub::Speaker& s = sp.list[at - 1];
    m_volume_target = s.id;
    ESP_LOGI(TAG, "Volume keys: %s", s.name.c_str());
    // A speaker that hasn't said its level: one LED, so the colour still shows.
    LedService::showLevel(s.volume >= 0 ? s.volume : 1, BLUE_LED);
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
