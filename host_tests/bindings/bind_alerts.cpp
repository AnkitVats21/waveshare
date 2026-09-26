#include "bindings.h"
#include "audio_core/AlertMixer.h"
#include "audio_core/AlertTones.h"
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>
#include <cstring>

namespace {

AlertClipPtr makeClip(const std::vector<int16_t>& pcm) {
    std::shared_ptr<AlertClip> clip = AlertClip::create(pcm.size());
    if (clip) std::memcpy(clip->data(), pcm.data(), pcm.size() * sizeof(int16_t));
    return clip;
}

std::vector<int16_t> clipSamples(const AlertClipPtr& clip) {
    if (!clip) return {};
    return std::vector<int16_t>(clip->data(), clip->data() + clip->size());
}

} // namespace

void init_alerts(nb::module_& m) {
    nb::class_<AlertMixer>(m, "AlertMixer")
        .def(nb::init<>())
        .def_ro_static("SLOTS", &AlertMixer::SLOTS)
        .def_ro_static("FADE_SAMPLES", &AlertMixer::FADE_SAMPLES)
        .def("set_clip", [](AlertMixer& self, size_t slot, const std::vector<int16_t>& pcm) {
            self.setClip(slot, pcm.empty() ? nullptr : makeClip(pcm));
        })
        .def("clip", [](AlertMixer& self, size_t slot) { return clipSamples(self.clip(slot)); })
        .def("set_gain", &AlertMixer::setGain)
        .def("gain", &AlertMixer::gain)
        .def("set_enabled", &AlertMixer::setEnabled)
        .def("enabled", &AlertMixer::enabled)
        .def("play", &AlertMixer::play, nb::arg("slot"), nb::arg("force") = false)
        .def("stop", &AlertMixer::stop)
        .def("active", &AlertMixer::active)
        .def("current", &AlertMixer::current)
        // Returns (samples, event) with event "", "started" or "ended".
        .def("render", [](AlertMixer& self, size_t n) {
            std::vector<int16_t> out(n);
            AlertMixer::Event ev;
            size_t got = self.render(out.data(), n, ev);
            out.resize(got);
            const char* name = ev == AlertMixer::Event::Started ? "started"
                             : ev == AlertMixer::Event::Ended   ? "ended" : "";
            return std::make_pair(out, std::string(name));
        });

    m.def("synthesize_tones", [](const std::vector<std::tuple<float, int, int, int>>& notes, uint32_t rate) {
        std::vector<ToneNote> v;
        for (auto& [hz, amp, ms, fade] : notes) {
            v.push_back({hz, (int16_t)amp, (uint16_t)ms, (uint16_t)fade});
        }
        return clipSamples(synthesizeTones(v.data(), v.size(), rate));
    });

    m.def("fade_edges", [](const std::vector<int16_t>& pcm, size_t in, size_t out) {
        std::shared_ptr<AlertClip> clip = AlertClip::create(pcm.size());
        std::memcpy(clip->data(), pcm.data(), pcm.size() * sizeof(int16_t));
        clip->fadeEdges(in, out);
        return clipSamples(clip);
    });
}
