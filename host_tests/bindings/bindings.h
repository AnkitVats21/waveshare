#pragma once
#include <nanobind/nanobind.h>

namespace nb = nanobind;

void init_sysdb(nb::module_& m);
void init_buffers(nb::module_& m);
