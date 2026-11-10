#pragma once

#include "core_sysdb/ReactorTask.h"
#include "hal/input/ExpanderKeyInput.h"
#include "app/input/KeyGesture.h"
#include <string>

class MusicPlaybackService;
class AudioRecorder;

/**
 * @brief Polls the keys and runs their actions (app/README.md, "Keys").
 *
 * Each key's gestures come from a KeyGesture; what they do is the BINDINGS
 * table. While an alarm rings or is snoozed, a press belongs to it: key 2
 * stops the alarm, the others snooze it, and nothing else runs for that press.
 * Key 1 is the chip's reset line and can't be read, so it isn't polled.
 *
 * The volume keys step the board's volume, or a hub speaker's (an Orbit
 * speaker) after a long press of key 4 picks it; the ring shows the level,
 * white for the board, blue for a speaker. A speaker that leaves the hub
 * gives the keys back to the board.
 */
class KeyService : public ReactorTask {
public:
    KeyService(ExpanderKeyInput& input, MusicPlaybackService& music, AudioRecorder& recorder);

    // Pure writer: watches no state.
    void onStateChanged(ComponentMask, const SystemState&) override {}

protected:
    void run() override;

private:
    using KeyId = ExpanderKeyInput::KeyId;
    using Event = Input::KeyGesture::Event;

    // A key's actions; nullptr for none. A down action that returns true
    // takes the whole press: no click or long press follows it.
    struct Binding {
        KeyId key;
        bool (KeyService::*down)();
        void (KeyService::*click)();
        void (KeyService::*longPress)();
    };
    static constexpr size_t KEY_COUNT = 4;   // keys 2-5
    static const Binding BINDINGS[KEY_COUNT];

    struct Key {
        Input::KeyGesture gesture;
        bool taken = false;   // by the alarm or the down action, until release
    };

    void handle(const Binding& b, Key& k, Event e);
    bool alarmTakes(KeyId key);

    // Actions
    bool stopRecording();
    void startProcessedRecording();
    void startStereoRecording();
    void volumeUp();
    void volumeDown();
    void stepVolume(int delta);
    void togglePlay();
    void nextVolumeTarget();
    void nextTrack();
    void previousTrack();

    ExpanderKeyInput& m_input;
    MusicPlaybackService& m_music;
    AudioRecorder& m_recorder;
    Key m_keys[KEY_COUNT];
    std::string m_volume_target;   // a hub speaker's id; empty: the board

    static constexpr const char* TAG = "KeySvc";
};
