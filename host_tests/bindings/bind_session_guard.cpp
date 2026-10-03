#include "bindings.h"
#include "app/audio/SessionGuard.h"

void init_session_guard(nb::module_& m) {
    using G = App::SessionGuard;
    nb::enum_<G::Verdict>(m, "GuardVerdict")
        .value("Ok", G::Verdict::Ok)
        .value("SoundNoWords", G::Verdict::SoundNoWords)
        .value("ReplyHoldExpired", G::Verdict::ReplyHoldExpired);
    nb::class_<G>(m, "SessionGuard")
        .def(nb::init<>())
        .def("reset", &G::reset)
        .def("note_speech", &G::noteSpeech)
        .def("due_for_check", &G::dueForCheck)
        .def("hold_expired", &G::holdExpired)
        .def("check", &G::check)
        .def_prop_ro_static("NONE", [](nb::handle) { return (int64_t)G::kNone; });
    m.attr("SOUND_NO_WORDS_MS") = (int64_t)App::SessionLimits::SOUND_NO_WORDS_MS;
    m.attr("SPEECH_GAP_RESET_MS") = (int64_t)App::SessionLimits::SPEECH_GAP_RESET_MS;
    m.attr("REPLY_HOLD_MAX_MS") = (int64_t)App::SessionLimits::REPLY_HOLD_MAX_MS;
}
