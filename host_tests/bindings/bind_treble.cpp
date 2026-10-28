#include "bindings.h"
#include "audio_core/TrebleFilter.h"

#include <nanobind/stl/vector.h>

void init_treble(nb::module_& m) {
    nb::class_<TrebleFilter>(m, "TrebleFilter")
        .def(nb::init<>())
        .def("set", &TrebleFilter::set)
        .def("db", &TrebleFilter::db)
        .def("reset", &TrebleFilter::reset)
        .def("process", [](TrebleFilter& f, const std::vector<float>& in) {
            std::vector<float> out(in.size());
            for (size_t i = 0; i < in.size(); ++i) out[i] = f.process(in[i]);
            return out;
        });
    m.attr("TREBLE_MIN_DB") = TrebleFilter::MIN_DB;
    m.attr("TREBLE_CORNER_HZ") = TrebleFilter::CORNER_HZ;
}
