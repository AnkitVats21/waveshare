#include "bindings.h"
#include "core_sysdb/EmbeddedSysDb.h"
#include "core_sysdb/SysDbTypes.h"
#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>
#include <nanobind/stl/pair.h>

namespace nb = nanobind;

void init_sysdb(nb::module_& m) {
    // ── STAR Enums ──
    nb::enum_<FieldAccess>(m, "FieldAccess")
        .value("ReadOnly", FieldAccess::ReadOnly)
        .value("Writable", FieldAccess::Writable)
        .value("PiOrigin", FieldAccess::PiOrigin)
        .export_values();

    nb::enum_<ComponentId>(m, "ComponentId")
        .value("SYSTEM", ComponentId::SYSTEM)
        .value("AUDIO", ComponentId::AUDIO)
        .value("PIPELINE", ComponentId::PIPELINE)
        .value("ASSISTANT", ComponentId::ASSISTANT)
        .value("LED", ComponentId::LED)
        .value("ALARM", ComponentId::ALARM)
        .value("BLUETOOTH", ComponentId::BLUETOOTH)
        .value("MEDIA", ComponentId::MEDIA)
        .export_values();

    nb::enum_<MediaOutputTarget>(m, "MediaOutputTarget")
        .value("LOCAL", MediaOutputTarget::LOCAL)
        .value("PI_BT", MediaOutputTarget::PI_BT)
        .export_values();

    nb::enum_<MediaCmdId>(m, "MediaCmdId")
        .value("NONE", MediaCmdId::NONE)
        .value("PLAY", MediaCmdId::PLAY)
        .value("PAUSE", MediaCmdId::PAUSE)
        .value("RESUME", MediaCmdId::RESUME)
        .value("STOP", MediaCmdId::STOP)
        .value("NEXT", MediaCmdId::NEXT)
        .value("PREVIOUS", MediaCmdId::PREVIOUS)
        .value("SEEK", MediaCmdId::SEEK)
        .export_values();

    // ── Existing Application Enums ──
    nb::enum_<AssistantState>(m, "AssistantState")
        .value("Idle", AssistantState::Idle)
        .value("StartingSession", AssistantState::StartingSession)
        .value("Connecting", AssistantState::Connecting)
        .value("StreamingUserAudio", AssistantState::StreamingUserAudio)
        .value("AssistantSpeaking", AssistantState::AssistantSpeaking)
        .value("WaitingForFollowup", AssistantState::WaitingForFollowup)
        .value("Closing", AssistantState::Closing)
        .value("ErrorCooldown", AssistantState::ErrorCooldown)
        .export_values();

    nb::enum_<AssistantVisualState>(m, "AssistantVisualState")
        .value("Idle", AssistantVisualState::Idle)
        .value("Listening", AssistantVisualState::Listening)
        .value("Connecting", AssistantVisualState::Connecting)
        .value("Speaking", AssistantVisualState::Speaking)
        .value("Thinking", AssistantVisualState::Thinking)
        .value("Offline", AssistantVisualState::Offline)
        .value("Recovering", AssistantVisualState::Recovering)
        .value("RateLimited", AssistantVisualState::RateLimited)
        .value("Error", AssistantVisualState::Error)
        .export_values();

    nb::enum_<PipelineMode>(m, "PipelineMode")
        .value("WAKE_IDLE", PipelineMode::WAKE_IDLE)
        .value("GEMINI_LIVE", PipelineMode::GEMINI_LIVE)
        .export_values();

    nb::enum_<NetworkState>(m, "NetworkState")
        .value("Disconnected", NetworkState::Disconnected)
        .value("Connecting", NetworkState::Connecting)
        .value("Connected", NetworkState::Connected)
        .value("Failed", NetworkState::Failed)
        .export_values();

    nb::enum_<MediaPlaybackState>(m, "MediaPlaybackState")
        .value("IDLE", MediaPlaybackState::IDLE)
        .value("RESOLVING", MediaPlaybackState::RESOLVING)
        .value("BUFFERING", MediaPlaybackState::BUFFERING)
        .value("PLAYING", MediaPlaybackState::PLAYING)
        .value("PAUSED", MediaPlaybackState::PAUSED)
        .value("ERROR_STATE", MediaPlaybackState::ERROR_STATE)
        .export_values();

    nb::enum_<WsState>(m, "WsState")
        .value("DISCONNECTED", WsState::DISCONNECTED)
        .value("CONNECTING", WsState::CONNECTING)
        .value("CONNECTED", WsState::CONNECTED)
        .value("GOING_AWAY", WsState::GOING_AWAY)
        .value("ERROR_STATE", WsState::ERROR_STATE)
        .export_values();

    // ── ComponentMask Constants ──
    auto comp = m.def_submodule("COMP", "Component change bitmasks");
    comp.attr("SYSTEM") = COMP::SYSTEM;
    comp.attr("AUDIO") = COMP::AUDIO;
    comp.attr("PIPELINE") = COMP::PIPELINE;
    comp.attr("ASSISTANT") = COMP::ASSISTANT;
    comp.attr("LED") = COMP::LED;
    comp.attr("ALARM") = COMP::ALARM;
    comp.attr("BLUETOOTH") = COMP::BLUETOOTH;
    comp.attr("BLUETOOTH") = COMP::BLUETOOTH;
    comp.attr("MEDIA") = COMP::MEDIA;
    comp.attr("ALL") = COMP::ALL;

    // ── Field Tag Namespaces ──
    auto tag_system = m.def_submodule("TAG_SYSTEM");
    tag_system.attr("wifi_connected") = static_cast<uint8_t>(TAG_SYSTEM::wifi_connected);
    tag_system.attr("network_state") = static_cast<uint8_t>(TAG_SYSTEM::network_state);
    tag_system.attr("ap_active") = static_cast<uint8_t>(TAG_SYSTEM::ap_active);
    tag_system.attr("wifi_ssid") = static_cast<uint8_t>(TAG_SYSTEM::wifi_ssid);
    tag_system.attr("wifi_password") = static_cast<uint8_t>(TAG_SYSTEM::wifi_password);
    tag_system.attr("wifi_apply_creds") = static_cast<uint8_t>(TAG_SYSTEM::wifi_apply_creds);

    auto tag_audio = m.def_submodule("TAG_AUDIO");
    tag_audio.attr("mic_gain_db") = static_cast<uint8_t>(TAG_AUDIO::mic_gain_db);
    tag_audio.attr("speaker_volume") = static_cast<uint8_t>(TAG_AUDIO::speaker_volume);
    tag_audio.attr("mic_enabled") = static_cast<uint8_t>(TAG_AUDIO::mic_enabled);
    tag_audio.attr("assistant_speaking") = static_cast<uint8_t>(TAG_AUDIO::assistant_speaking);

    auto tag_media = m.def_submodule("TAG_MEDIA");
    tag_media.attr("state") = static_cast<uint8_t>(TAG_MEDIA::state);
    tag_media.attr("active_song_id") = static_cast<uint8_t>(TAG_MEDIA::active_song_id);
    tag_media.attr("title") = static_cast<uint8_t>(TAG_MEDIA::title);
    tag_media.attr("artist") = static_cast<uint8_t>(TAG_MEDIA::artist);
    tag_media.attr("position_ms") = static_cast<uint8_t>(TAG_MEDIA::position_ms);
    tag_media.attr("duration_ms") = static_cast<uint8_t>(TAG_MEDIA::duration_ms);
    tag_media.attr("seekable") = static_cast<uint8_t>(TAG_MEDIA::seekable);
    tag_media.attr("repeat_mode") = static_cast<uint8_t>(TAG_MEDIA::repeat_mode);
    tag_media.attr("is_ducked") = static_cast<uint8_t>(TAG_MEDIA::is_ducked);
    tag_media.attr("autoplay_enabled") = static_cast<uint8_t>(TAG_MEDIA::autoplay_enabled);
    tag_media.attr("cache_downloads") = static_cast<uint8_t>(TAG_MEDIA::cache_downloads);
    tag_media.attr("output_target") = static_cast<uint8_t>(TAG_MEDIA::output_target);
    tag_media.attr("pending_command") = static_cast<uint8_t>(TAG_MEDIA::pending_command);

    auto tag_bt = m.def_submodule("TAG_BLUETOOTH");
    tag_bt.attr("connected") = static_cast<uint8_t>(TAG_BLUETOOTH::connected);
    tag_bt.attr("device_name") = static_cast<uint8_t>(TAG_BLUETOOTH::device_name);
    tag_bt.attr("mac_address") = static_cast<uint8_t>(TAG_BLUETOOTH::mac_address);
    tag_bt.attr("rssi") = static_cast<uint8_t>(TAG_BLUETOOTH::rssi);

    // ── SystemState ──
    nb::class_<SystemState>(m, "SystemState")
        .def(nb::init<>())
        .def_prop_rw("wifi_connected",
            [](const SystemState& s) { return s.system.wifi_connected; },
            [](SystemState& s, bool v) { s.system.wifi_connected = v; })
        .def_prop_rw("ap_active",
            [](const SystemState& s) { return s.system.ap_active; },
            [](SystemState& s, bool v) { s.system.ap_active = v; })
        .def_prop_rw("wifi_ssid",
            [](const SystemState& s) { return std::string(s.system.wifi_ssid); },
            [](SystemState& s, const std::string& v) {
                strncpy(s.system.wifi_ssid, v.c_str(), sizeof(s.system.wifi_ssid) - 1);
                s.system.wifi_ssid[sizeof(s.system.wifi_ssid) - 1] = '\0';
            })
        .def_prop_rw("wifi_password",
            [](const SystemState& s) { return std::string(s.system.wifi_password); },
            [](SystemState& s, const std::string& v) {
                strncpy(s.system.wifi_password, v.c_str(), sizeof(s.system.wifi_password) - 1);
                s.system.wifi_password[sizeof(s.system.wifi_password) - 1] = '\0';
            })
        .def_prop_rw("wifi_apply_creds",
            [](const SystemState& s) { return s.system.wifi_apply_creds; },
            [](SystemState& s, bool v) { s.system.wifi_apply_creds = v; })
        .def_prop_rw("speaker_volume",
            [](const SystemState& s) { return s.audio.speaker_volume; },
            [](SystemState& s, int v) { s.audio.speaker_volume = v; })
        .def_prop_rw("mic_gain_db",
            [](const SystemState& s) { return s.audio.mic_gain_db; },
            [](SystemState& s, float v) { s.audio.mic_gain_db = v; })
        .def_prop_rw("mic_enabled",
            [](const SystemState& s) { return s.audio.mic_enabled; },
            [](SystemState& s, bool v) { s.audio.mic_enabled = v; })
        .def_prop_rw("assistant_speaking",
            [](const SystemState& s) { return s.audio.assistant_speaking; },
            [](SystemState& s, bool v) { s.audio.assistant_speaking = v; })
        .def_prop_rw("turn_complete_pending",
            [](const SystemState& s) { return s.audio.turn_complete_pending; },
            [](SystemState& s, bool v) { s.audio.turn_complete_pending = v; })
        .def_prop_rw("pipeline_mode",
            [](const SystemState& s) { return s.pipeline.mode; },
            [](SystemState& s, PipelineMode v) { s.pipeline.mode = v; })
        .def_prop_rw("session_state",
            [](const SystemState& s) { return s.assistant.session_state; },
            [](SystemState& s, AssistantState v) { s.assistant.session_state = v; })
        .def_prop_rw("visual_state",
            [](const SystemState& s) { return s.assistant.visual_state; },
            [](SystemState& s, AssistantVisualState v) { s.assistant.visual_state = v; })
        .def_prop_rw("media_state",
            [](const SystemState& s) { return s.media.state; },
            [](SystemState& s, MediaPlaybackState v) { s.media.state = v; })
        .def_prop_rw("is_ducked",
            [](const SystemState& s) { return s.media.is_ducked; },
            [](SystemState& s, bool v) { s.media.is_ducked = v; })
        .def_prop_rw("autoplay_enabled",
            [](const SystemState& s) { return s.media.autoplay_enabled; },
            [](SystemState& s, bool v) { s.media.autoplay_enabled = v; })
        .def_prop_rw("bluetooth_connected",
            [](const SystemState& s) { return s.bluetooth.connected; },
            [](SystemState& s, bool v) { s.bluetooth.connected = v; })
        .def_prop_rw("companion_connected",
            [](const SystemState& s) { return s.bluetooth.connected; },
            [](SystemState& s, bool v) { s.bluetooth.connected = v; })
        .def_prop_rw("companion_settled",
            [](const SystemState& s) { return s.bluetooth.connected; },
            [](SystemState& s, bool v) { s.bluetooth.connected = v; })
        .def_prop_rw("output_target",
            [](const SystemState& s) { return s.media.output_target; },
            [](SystemState& s, MediaOutputTarget v) { s.media.output_target = v; })
        .def_prop_rw("active_song_id",
            [](const SystemState& s) { return std::string(s.media.active_song_id); },
            [](SystemState& s, const std::string& v) {
                strncpy(s.media.active_song_id, v.c_str(), sizeof(s.media.active_song_id) - 1);
                s.media.active_song_id[sizeof(s.media.active_song_id) - 1] = '\0';
            })
        .def_prop_rw("media_title",
            [](const SystemState& s) { return std::string(s.media.title); },
            [](SystemState& s, const std::string& v) {
                strncpy(s.media.title, v.c_str(), sizeof(s.media.title) - 1);
                s.media.title[sizeof(s.media.title) - 1] = '\0';
            });

    // ── EmbeddedSysDb ──
    nb::class_<EmbeddedSysDb>(m, "EmbeddedSysDb")
        .def_static("get_instance", &EmbeddedSysDb::getInstance, nb::rv_policy::reference)
        .def("snapshot", &EmbeddedSysDb::snapshot, nb::call_guard<nb::gil_scoped_release>())
        .def("mutate", [](EmbeddedSysDb& self, nb::callable fn) {
            nb::gil_scoped_release release_gil;
            self.mutate([&fn](SystemState& s) {
                nb::gil_scoped_acquire acquire_gil;
                fn(nb::cast(&s, nb::rv_policy::reference));
            });
        })
        .def("wifi_connected", &EmbeddedSysDb::wifiConnected, nb::call_guard<nb::gil_scoped_release>())
        .def("network_state", &EmbeddedSysDb::networkState, nb::call_guard<nb::gil_scoped_release>())
        .def("speaker_volume", &EmbeddedSysDb::speakerVolume, nb::call_guard<nb::gil_scoped_release>())
        .def("mic_gain", &EmbeddedSysDb::micGain, nb::call_guard<nb::gil_scoped_release>())
        .def("assistant_speaking", &EmbeddedSysDb::assistantSpeaking, nb::call_guard<nb::gil_scoped_release>())
        .def("mic_enabled", &EmbeddedSysDb::micEnabled, nb::call_guard<nb::gil_scoped_release>())
        .def("session_state", &EmbeddedSysDb::sessionState, nb::call_guard<nb::gil_scoped_release>())
        .def("pipeline_mode", &EmbeddedSysDb::pipelineMode, nb::call_guard<nb::gil_scoped_release>())
        .def("ws_state", &EmbeddedSysDb::wsState, nb::call_guard<nb::gil_scoped_release>())
        .def("turn_complete_pending", &EmbeddedSysDb::turnCompletePending, nb::call_guard<nb::gil_scoped_release>())
        .def("alarm_playing", &EmbeddedSysDb::alarmPlaying, nb::call_guard<nb::gil_scoped_release>())
        .def("alarm_stop_requested", &EmbeddedSysDb::alarmStopRequested, nb::call_guard<nb::gil_scoped_release>())
        .def("media_state", &EmbeddedSysDb::mediaState, nb::call_guard<nb::gil_scoped_release>())
        .def("is_media_ducked", &EmbeddedSysDb::isMediaDucked, nb::call_guard<nb::gil_scoped_release>())
        .def("autoplay_enabled", &EmbeddedSysDb::autoplayEnabled, nb::call_guard<nb::gil_scoped_release>())
        .def("cache_downloads", &EmbeddedSysDb::cacheDownloads, nb::call_guard<nb::gil_scoped_release>())
        .def("bluetooth_connected", &EmbeddedSysDb::bluetoothConnected, nb::call_guard<nb::gil_scoped_release>())
        .def("hot_audio_flags", &EmbeddedSysDb::hotAudioFlags)
        .def("hot_assistant_speaking", &EmbeddedSysDb::hotAssistantSpeaking)
        .def("hot_turn_complete_pending", &EmbeddedSysDb::hotTurnCompletePending)
        .def("hot_companion_connected", &EmbeddedSysDb::hotBluetoothConnected)
        .def("hot_companion_settled", &EmbeddedSysDb::hotBluetoothSettled);
}
