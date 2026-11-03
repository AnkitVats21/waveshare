#include "bindings.h"
#include "voice_session/SessionRules.h"

#include <nanobind/stl/tuple.h>
#include <nanobind/stl/string.h>
#include <string>

void init_session_rules(nb::module_& m) {
    // decide() from keyword facts and rules; returns (end, window_ms, music).
    m.def("session_decide",
          [](int music_ok, int music_failed, int tools_ok, int tools_failed, bool hub_music_done,
             uint32_t followup_ms, bool end_after_failed_music) {
              voice::SessionFacts f;
              f.music_ok = uint8_t(music_ok);
              f.music_failed = uint8_t(music_failed);
              f.tools_ok = uint8_t(tools_ok);
              f.tools_failed = uint8_t(tools_failed);
              f.hub_music_done = hub_music_done;
              voice::Rules r;
              r.followup_ms = followup_ms;
              r.end_after_failed_music = end_after_failed_music;
              const voice::AfterReply d = voice::decide(f, r);
              const char* music = d.music == voice::MusicAfter::Quiet ? "quiet"
                                : d.music == voice::MusicAfter::Play  ? "play" : "hold";
              return std::make_tuple(d.end, d.window_ms, std::string(music));
          },
          nb::arg("music_ok") = 0, nb::arg("music_failed") = 0, nb::arg("tools_ok") = 0,
          nb::arg("tools_failed") = 0, nb::arg("hub_music_done") = false,
          nb::arg("followup_ms") = 60000, nb::arg("end_after_failed_music") = false);

    m.def("session_classify_error", [](int code, const std::string& reason) {
        return std::string(voice::errorKindName(voice::classifyError(code, reason.c_str())));
    });
    m.def("session_pick_model", [](const std::string& primary, bool fallback_active, const std::string& fallback) {
        voice::Rules r;
        r.fallback_model = fallback;
        return voice::pickModel(primary, fallback_active, r);
    });
}
