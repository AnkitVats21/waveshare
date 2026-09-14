#include "bindings.h"
#include "core_sysdb/BufferManager.h"
#include <nanobind/stl/string.h>
#include <cstring>

void init_buffers(nb::module_& m) {
    nb::class_<BufferManager>(m, "BufferManager")
        .def_static("get_instance", &BufferManager::getInstance, nb::rv_policy::reference)
        .def("init_all", &BufferManager::initAll)
        .def("send", [](BufferManager& self, uint8_t id, nb::bytes data, uint32_t timeout_ms) {
            return self.send(id, data.c_str(), data.size(), timeout_ms);
        }, nb::arg("id"), nb::arg("data"), nb::arg("timeout_ms") = 0)
        .def("receive", [](BufferManager& self, uint8_t id, size_t max_bytes, uint32_t timeout_ms) -> nb::bytes {
            size_t out_len = 0;
            void* item = self.receive(id, &out_len, timeout_ms, max_bytes);
            if (!item || out_len == 0) {
                self.returnItem(id, item);
                return nb::bytes("", 0);
            }
            nb::bytes res(static_cast<const char*>(item), out_len);
            self.returnItem(id, item);
            return res;
        }, nb::arg("id"), nb::arg("max_bytes") = 0, nb::arg("timeout_ms") = 0)
        .def("flush", &BufferManager::flush)
        .def("destroy", &BufferManager::destroy)
        .def("size", &BufferManager::size)
        .def("get_used_bytes", &BufferManager::getUsedBytes)
        .def("dump_stats", &BufferManager::dumpStats);

    m.def("register_buffer", [](const std::string& name, size_t bytes, int type) -> int {
        BufferManager::BufferId id = BufferManager::INVALID;
        BufferManager::getInstance().registerDescriptor(
            id, strdup(name.c_str()), bytes, static_cast<RingbufferType_t>(type));
        return id;
    }, nb::arg("name"), nb::arg("bytes"), nb::arg("type") = 2);
}
