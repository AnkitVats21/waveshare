#include "bindings.h"
#include "core_sysdb/EmbeddedSysDb.h"
#include "core_sysdb/SystemState.h"
#include "core_sysdb/app_types.h"

#include <nanobind/stl/string.h>
#include <nanobind/stl/function.h>
#include <cstring>

void init_sysdb(nb::module_& m) {
    // ── Enums ──
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
        .value("RTP_REMOTE", PipelineMode::RTP_REMOTE)
        .value("RTP_WAKEWORD", PipelineMode::RTP_WAKEWORD)
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

    // ── COMP mask constants ──
    auto comp = m.def_submodule("COMP", "Component bitmasks");
    comp.attr("SYSTEM") = COMP::SYSTEM;
    comp.attr("AUDIO") = COMP::AUDIO;
    comp.attr("PIPELINE") = COMP::PIPELINE;
    comp.attr("ASSISTANT") = COMP::ASSISTANT;
    comp.attr("LED") = COMP::LED;
    comp.attr("MQTT") = COMP::MQTT;
    comp.attr("ALARM") = COMP::ALARM;
    comp.attr("BT_COMPANION") = COMP::BT_COMPANION;
    comp.attr("MEDIA") = COMP::MEDIA;
    comp.attr("ALL") = COMP::ALL;

    // ── SystemState ──
    nb::class_<SystemState>(m, "SystemState")
        .def(nb::init<>())
        .def_prop_rw("wifi_connected",
            [](const SystemState& s) { return s.system.wifi_connected; },
            [](SystemState& s, bool v) { s.system.wifi_connected = v; })
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
        .def_prop_rw("companion_connected",
            [](const SystemState& s) { return s.bt_companion.connected; },
            [](SystemState& s, bool v) { s.bt_companion.connected = v; })
        .def_prop_rw("companion_settled",
            [](const SystemState& s) { return s.bt_companion.link_settled; },
            [](SystemState& s, bool v) { s.bt_companion.link_settled = v; })
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
        .def("snapshot", &EmbeddedSysDb::snapshot)
        .def("mutate", [](EmbeddedSysDb& self, nb::callable fn) {
            self.mutate([&fn](SystemState& s) {
                fn(nb::cast(&s, nb::rv_policy::reference));
            });
        })
        .def("wifi_connected", &EmbeddedSysDb::wifiConnected)
        .def("network_state", &EmbeddedSysDb::networkState)
        .def("speaker_volume", &EmbeddedSysDb::speakerVolume)
        .def("mic_gain", &EmbeddedSysDb::micGain)
        .def("assistant_speaking", &EmbeddedSysDb::assistantSpeaking)
        .def("mic_enabled", &EmbeddedSysDb::micEnabled)
        .def("session_state", &EmbeddedSysDb::sessionState)
        .def("pipeline_mode", &EmbeddedSysDb::pipelineMode)
        .def("ws_state", &EmbeddedSysDb::wsState)
        .def("turn_complete_pending", &EmbeddedSysDb::turnCompletePending)
        .def("alarm_playing", &EmbeddedSysDb::alarmPlaying)
        .def("alarm_stop_requested", &EmbeddedSysDb::alarmStopRequested)
        .def("media_state", &EmbeddedSysDb::mediaState)
        .def("is_media_ducked", &EmbeddedSysDb::isMediaDucked)
        .def("autoplay_enabled", &EmbeddedSysDb::autoplayEnabled)
        .def("cache_downloads", &EmbeddedSysDb::cacheDownloads)
        .def("bt_companion_connected", &EmbeddedSysDb::btCompanionConnected)
        .def("bt_companion_link_settled", &EmbeddedSysDb::btCompanionLinkSettled)
        .def("hot_audio_flags", &EmbeddedSysDb::hotAudioFlags)
        .def("hot_assistant_speaking", &EmbeddedSysDb::hotAssistantSpeaking)
        .def("hot_turn_complete_pending", &EmbeddedSysDb::hotTurnCompletePending)
        .def("hot_companion_connected", &EmbeddedSysDb::hotCompanionConnected)
        .def("hot_companion_settled", &EmbeddedSysDb::hotCompanionSettled);
}
