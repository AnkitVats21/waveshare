#include "bindings.h"
#include "audio_core/ToneControl.h"

#include <nanobind/stl/vector.h>

void init_tone(nb::module_& m) {
    nb::class_<ToneControl>(m, "ToneControl")
        .def(nb::init<>())
        .def("set", &ToneControl::set)
        .def("bass_db", &ToneControl::bassDb)
        .def("treble_db", &ToneControl::trebleDb)
        .def("reset", &ToneControl::reset)
        .def("process", [](ToneControl& t, const std::vector<float>& in) {
            std::vector<float> out(in.size());
            for (size_t i = 0; i < in.size(); ++i) out[i] = t.process(in[i]);
            return out;
        });
    m.attr("TONE_MIN_DB") = ToneControl::MIN_DB;
    m.attr("TONE_MAX_DB") = ToneControl::MAX_DB;
    m.attr("TONE_BASS_HZ") = ToneControl::BASS_HZ;
    m.attr("TONE_TREBLE_HZ") = ToneControl::TREBLE_HZ;
}
