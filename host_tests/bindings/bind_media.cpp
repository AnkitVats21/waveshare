#include "bindings.h"
#include "media_player/WebmSeek.h"
#include "app/audio/recording/RecordingProbe.h"
#include <nanobind/stl/string.h>
#include <string>

namespace {

std::string statusName(Media::CuesStatus s) {
    switch (s) {
    case Media::CuesStatus::Found: return "found";
    case Media::CuesStatus::NeedMore: return "need_more";
    case Media::CuesStatus::NotFound: return "not_found";
    case Media::CuesStatus::Invalid: return "invalid";
    default: return "";
    }
}

} // namespace

void init_media(nb::module_& m) {
    // (status, [(time_ms, offset), ...], need); status is "found", "need_more",
    // "not_found" or "invalid".
    m.def("parse_webm_cues", [](nb::bytes data) {
        auto r = Media::parseWebmCues(reinterpret_cast<const uint8_t*>(data.c_str()), data.size());
        nb::list cues;
        for (const auto& c : r.cues) cues.append(nb::make_tuple(c.time_ms, c.offset));
        return nb::make_tuple(statusName(r.status), cues, r.need);
    });
    m.def("cue_at_or_before", [](nb::list pairs, uint32_t time_ms) {
        std::vector<Media::CuePoint> cues;
        for (auto p : pairs) {
            auto t = nb::cast<nb::tuple>(p);
            cues.push_back({nb::cast<uint32_t>(t[0]), nb::cast<uint32_t>(t[1])});
        }
        const auto& c = Media::cueAtOrBefore(cues, time_ms);
        return nb::make_tuple(c.time_ms, c.offset);
    });
    m.attr("MAX_CUES") = Media::MAX_CUES;

    // "skip", "preroll" or "play".
    m.def("seek_action", [](uint32_t block_ms, uint32_t target_ms) -> std::string {
        switch (Media::seekAction(block_ms, target_ms)) {
        case Media::SeekAction::Skip: return "skip";
        case Media::SeekAction::Preroll: return "preroll";
        default: return "play";
        }
    });
    m.attr("SEEK_PREROLL_MS") = Media::SEEK_PREROLL_MS;
    // The relative timecode, or None if the header is incomplete.
    m.def("read_block_timecode", [](nb::bytes payload) -> nb::object {
        int16_t rel = 0;
        if (!Media::readBlockTimecode(reinterpret_cast<const uint8_t*>(payload.c_str()), payload.size(), rel)) {
            return nb::none();
        }
        return nb::int_(rel);
    });
    // (found, at)
    m.def("find_cluster", [](nb::bytes data) {
        const auto r = Media::findCluster(reinterpret_cast<const uint8_t*>(data.c_str()), data.size());
        return nb::make_tuple(r.found, r.at);
    });

    // (duration_ms, sample_rate, channels), or None.
    m.def("probe_opus", [](nb::bytes head, nb::bytes tail) -> nb::object {
        RecordingProbe::Info i;
        if (!RecordingProbe::probeOpus(reinterpret_cast<const uint8_t*>(head.c_str()), head.size(),
                                       reinterpret_cast<const uint8_t*>(tail.c_str()), tail.size(), i)) {
            return nb::none();
        }
        return nb::make_tuple(i.duration_ms, i.sample_rate, i.channels);
    });
    m.def("probe_wav", [](nb::bytes head, uint64_t file_size) -> nb::object {
        RecordingProbe::Info i;
        if (!RecordingProbe::probeWav(reinterpret_cast<const uint8_t*>(head.c_str()), head.size(), file_size, i)) {
            return nb::none();
        }
        return nb::make_tuple(i.duration_ms, i.sample_rate, i.channels);
    });
    m.attr("PROBE_HEAD_BYTES") = RecordingProbe::HEAD_BYTES;
    m.attr("PROBE_TAIL_BYTES") = RecordingProbe::TAIL_BYTES;
}
