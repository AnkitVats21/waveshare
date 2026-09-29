#include "bindings.h"
#include "services/alarm/AlarmRing.h"
#include "services/alarm/AlarmSchedule.h"
#include "services/alarm/AudioSniff.h"
#include <cstdlib>
#include <ctime>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <string>

using Services::AlarmRing;

namespace {

std::string actionName(AlarmRing::Action a) {
    switch (a) {
    case AlarmRing::Action::PlaySong: return "song";
    case AlarmRing::Action::PlayBuiltin: return "builtin";
    case AlarmRing::Action::Silence: return "silence";
    case AlarmRing::Action::Finish: return "finish";
    default: return "";
    }
}

} // namespace

void init_alarm(nb::module_& m) {
    // Process timezone (POSIX TZ string), as TimeSyncHelper::applyTimezone does.
    m.def("set_tz", [](const std::string& tz) {
        setenv("TZ", tz.c_str(), 1);
        tzset();
    });
    m.def("next_fire", [](int hour, int minute, int days, uint32_t at, int64_t after) {
        Services::AlarmWhen w;
        w.hour = uint8_t(hour);
        w.minute = uint8_t(minute);
        w.days = uint8_t(days);
        w.at = at;
        return Services::nextFire(w, after);
    }, nb::arg("hour"), nb::arg("minute"), nb::arg("days") = 0, nb::arg("at") = 0, nb::arg("after") = 0);
    // (kind, value): kind is "builtin", "file" or "song".
    m.def("parse_tone", [](const std::string& tone) {
        const Services::AlarmTone t = Services::parseTone(tone);
        const char* kind = t.kind == Services::AlarmTone::Kind::File ? "file"
                         : t.kind == Services::AlarmTone::Kind::Song ? "song" : "builtin";
        return std::make_pair(std::string(kind), t.value);
    });
    m.def("is_valid_tone_file_name", &Services::isValidToneFileName);
    m.def("briefing_tone", &Services::briefingTone);
    // (playable, format) from a file's first bytes.
    m.def("sniff_audio", [](nb::bytes data) {
        const Services::AudioSniff r =
            Services::sniffAudio(reinterpret_cast<const uint8_t*>(data.c_str()), data.size());
        return std::make_pair(r.playable, std::string(r.format));
    });
    // None if not understood.
    m.def("parse_days", [](const std::string& text) -> nb::object {
        uint8_t days = 0;
        if (!Services::parseDays(text, days)) return nb::none();
        return nb::int_(days);
    });
    m.def("format_days", [](int days) { return Services::formatDays(uint8_t(days)); });
    m.def("resolve_day", [](const std::string& day, int hour, int minute, int64_t now) {
        return Services::resolveDay(day, hour, minute, now);
    });
    m.attr("DAY_INVALID") = Services::DAY_INVALID;
    m.attr("DAY_PAST") = Services::DAY_PAST;

    // Actions come back as "", "song", "builtin", "silence" or "finish".
    nb::class_<AlarmRing>(m, "AlarmRing")
        .def("__init__", [](AlarmRing* self, uint32_t ring_limit_ms, uint32_t snooze_ms, uint32_t watchdog_ms) {
            AlarmRing::Config cfg;
            cfg.ring_limit_ms = ring_limit_ms;
            cfg.snooze_ms = snooze_ms;
            cfg.start_watchdog_ms = watchdog_ms;
            new (self) AlarmRing(cfg);
        }, nb::arg("ring_limit_ms") = 600000, nb::arg("snooze_ms") = 540000, nb::arg("watchdog_ms") = 3000)
        .def("fire", [](AlarmRing& r, int id, bool song, uint64_t t) { return actionName(r.fire(id, song, t)); })
        .def("stop", [](AlarmRing& r, uint64_t t) { return actionName(r.stop(t)); })
        .def("snooze", [](AlarmRing& r, uint64_t t) { return actionName(r.snooze(t)); })
        .def("song_progress", [](AlarmRing& r, uint64_t t) { return actionName(r.songProgress(t)); })
        .def("song_ended", [](AlarmRing& r, uint64_t t) { return actionName(r.songEnded(t)); })
        .def("song_failed", [](AlarmRing& r, uint64_t t) { return actionName(r.songFailed(t)); })
        .def("tick", [](AlarmRing& r, uint64_t t) { return actionName(r.tick(t)); })
        .def("state", [](AlarmRing& r) {
            switch (r.state()) {
            case AlarmRing::State::Ringing: return std::string("ringing");
            case AlarmRing::State::Snoozed: return std::string("snoozed");
            default: return std::string("idle");
            }
        })
        .def("source", [](AlarmRing& r) {
            switch (r.source()) {
            case AlarmRing::Source::Song: return std::string("song");
            case AlarmRing::Source::Builtin: return std::string("builtin");
            default: return std::string("");
            }
        })
        .def("last_end", [](AlarmRing& r) {
            switch (r.lastEnd()) {
            case AlarmRing::EndReason::Stopped: return std::string("stopped");
            case AlarmRing::EndReason::TimedOut: return std::string("timed_out");
            default: return std::string("");
            }
        })
        .def("alarm_id", &AlarmRing::alarmId)
        .def("snooze_count", &AlarmRing::snoozeCount)
        .def("snooze_left_ms", &AlarmRing::snoozeLeftMs)
        .def("ringing_ms", &AlarmRing::ringingMs);
}
