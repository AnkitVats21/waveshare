#pragma once
#include <nanobind/nanobind.h>

namespace nb = nanobind;

void init_sysdb(nb::module_& m);
void init_buffers(nb::module_& m);
void init_resampler(nb::module_& m);
void init_nexus_db(nb::module_& m);
void init_alerts(nb::module_& m);
void init_alarm(nb::module_& m);
void init_media(nb::module_& m);
void init_weather(nb::module_& m);
void init_profiles(nb::module_& m);
void init_mcp(nb::module_& m);
void init_framing(nb::module_& m);
void init_session_guard(nb::module_& m);
void init_tone(nb::module_& m);
