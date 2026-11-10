#include "bindings.h"
#include "app/input/KeyGesture.h"
#include <nanobind/stl/string.h>
#include <string>

namespace {

std::string eventName(Input::KeyGesture::Event e) {
    switch (e) {
    case Input::KeyGesture::Event::Down: return "down";
    case Input::KeyGesture::Event::LongPress: return "long";
    case Input::KeyGesture::Event::Click: return "click";
    default: return "";
    }
}

} // namespace

void init_keys(nb::module_& m) {
    using G = Input::KeyGesture;
    nb::class_<G>(m, "KeyGesture")
        .def(nb::init<>())
        .def("update", [](G& g, bool pressed, uint32_t now_ms) { return eventName(g.update(pressed, now_ms)); })
        .def("down", &G::down);
    m.attr("LONG_PRESS_MS") = G::LONG_PRESS_MS;
}
