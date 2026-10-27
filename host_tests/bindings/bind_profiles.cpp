#include "bindings.h"
#include "services/profiles/ProfileMatch.h"
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

void init_profiles(nb::module_& m) {
    m.def("profile_match", &ProfileMatch::match);
    m.def("profile_normalize", &ProfileMatch::normalize);
    m.def("profile_same_name", &ProfileMatch::sameName);
    m.attr("PROFILE_NONE") = ProfileMatch::NONE;
    m.attr("PROFILE_AMBIGUOUS") = ProfileMatch::AMBIGUOUS;
}
