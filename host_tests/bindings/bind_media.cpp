#include "bindings.h"
#include "media_player/WebmSeek.h"
#include "media_player/OggSeek.h"
#include "app/audio/recording/RecordingProbe.h"
#include <nanobind/stl/string.h>
#include <string>
#include <vector>
#include <algorithm>

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
    // The Segment Info Duration in ms, 0 if absent (or the parse stopped early).
    m.def("webm_duration_ms", [](nb::bytes data) {
        return Media::parseWebmCues(reinterpret_cast<const uint8_t*>(data.c_str()), data.size()).duration_ms;
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
    // Ogg seeking. (status, flags, serial, seq, granule, len); status "ok",
    // "need_more" or "not_page".
    m.def("read_ogg_page", [](nb::bytes data) {
        Media::OggPage p{};
        auto r = Media::readOggPage(reinterpret_cast<const uint8_t*>(data.c_str()), data.size(), p);
        const char* st = r == Media::OggPageRead::Ok ? "ok" : r == Media::OggPageRead::NeedMore ? "need_more" : "not_page";
        return nb::make_tuple(st, p.flags, p.serial, p.seq, p.granule, p.len);
    });
    // (serial, pre_skip, pages, header bytes) or None.
    m.def("parse_ogg_header", [](nb::bytes data) -> nb::object {
        Media::OggHeader h;
        if (!Media::parseOggHeader(reinterpret_cast<const uint8_t*>(data.c_str()), data.size(), h)) return nb::none();
        return nb::make_tuple(h.serial, h.pre_skip, h.pages,
                              nb::bytes(reinterpret_cast<const char*>(h.bytes.data()), h.bytes.size()));
    });
    // The file's header renumbered to run into first_audio_seq; b"" if not possible.
    m.def("renumber_ogg_header", [](nb::bytes file_head, uint32_t first_audio_seq) {
        Media::OggHeader h;
        std::vector<uint8_t> out;
        if (Media::parseOggHeader(reinterpret_cast<const uint8_t*>(file_head.c_str()), file_head.size(), h)) {
            out = Media::renumberHeader(h, first_audio_seq);
        }
        return nb::bytes(reinterpret_cast<const char*>(out.data()), out.size());
    });
    // Feeds `data` to an OggSeekScanner in `chunk`-byte pieces, as the decoder
    // does: (found, bytes dropped, start seq, discard frames).
    m.def("ogg_seek_scan", [](nb::bytes data, uint32_t serial, uint16_t pre_skip, uint32_t target_ms, size_t chunk) {
        Media::OggSeekScanner s(serial, pre_skip, target_ms);
        const auto* d = reinterpret_cast<const uint8_t*>(data.c_str());
        std::vector<uint8_t> buf;
        size_t dropped = 0;
        for (size_t at = 0; at < data.size() && !s.found(); at += chunk) {
            buf.insert(buf.end(), d + at, d + std::min(data.size(), at + chunk));
            const size_t drop = s.scan(buf.data(), buf.size());
            buf.erase(buf.begin(), buf.begin() + drop);
            dropped += drop;
        }
        return nb::make_tuple(s.found(), dropped, s.startSeq(), s.discardFrames());
    });
    m.def("ogg_page_start_granule", [](nb::bytes data) -> int64_t {
        Media::OggPage p{};
        const auto* d = reinterpret_cast<const uint8_t*>(data.c_str());
        if (Media::readOggPage(d, data.size(), p) != Media::OggPageRead::Ok) return -2;
        return Media::oggPageStartGranule(d, p);
    });
    m.attr("OGG_CONTINUED") = Media::OGG_CONTINUED;
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
