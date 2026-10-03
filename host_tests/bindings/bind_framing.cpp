#include "bindings.h"
#include "services/hub/UplinkFraming.h"
#include <nanobind/stl/vector.h>
#include <nanobind/stl/string.h>

namespace {

class PyOpusFrameBuilder {
public:
    explicit PyOpusFrameBuilder(size_t capacity)
        : m_buffer(capacity), m_builder(m_buffer.data(), capacity) {}

    void reset() { m_builder.reset(); }

    bool add_packet(nb::bytes packet) {
        return m_builder.addPacket(reinterpret_cast<const uint8_t*>(packet.c_str()), packet.size());
    }

    size_t packet_count() const { return m_builder.packetCount(); }
    size_t size() const { return m_builder.size(); }
    size_t capacity() const { return m_builder.capacity(); }
    bool empty() const { return m_builder.empty(); }

    nb::bytes data() const {
        return nb::bytes(reinterpret_cast<const char*>(m_builder.data()), m_builder.size());
    }

private:
    std::vector<uint8_t> m_buffer;
    Services::OpusFrameBuilder m_builder;
};

class PyPcmAccumulator {
public:
    PyPcmAccumulator() = default;

    void reset() { m_acc.reset(); }

    nb::list feed(nb::bytes data) {
        nb::list frames;
        const auto* d = reinterpret_cast<const uint8_t*>(data.c_str());
        m_acc.feed(d, data.size(), [&frames](const int16_t* frame_320) {
            frames.append(nb::bytes(reinterpret_cast<const char*>(frame_320), Services::BYTES_PER_FRAME));
        });
        return frames;
    }

    size_t remainder_bytes() const { return m_acc.remainderBytes(); }
    size_t remainder_samples() const { return m_acc.remainderSamples(); }

    nb::bytes flush_bytes() {
        std::vector<uint8_t> buf(Services::BYTES_PER_FRAME);
        size_t n = m_acc.flushBytes(buf.data());
        return nb::bytes(reinterpret_cast<const char*>(buf.data()), n);
    }

private:
    Services::PcmAccumulator m_acc;
};

} // namespace

void init_framing(nb::module_& m) {
    m.attr("MIC_FRAME_MAX")    = Services::MIC_FRAME_MAX;
    m.attr("CHANNEL_MIC")      = Services::CHANNEL_MIC;
    m.attr("FLAG_PCM")         = Services::FLAG_PCM;
    m.attr("FLAG_OPUS")        = Services::FLAG_OPUS;
    m.attr("SAMPLES_PER_FRAME") = Services::SAMPLES_PER_FRAME;
    m.attr("BYTES_PER_FRAME")   = Services::BYTES_PER_FRAME;

    nb::class_<PyPcmAccumulator>(m, "PcmAccumulator")
        .def(nb::init<>())
        .def("reset", &PyPcmAccumulator::reset)
        .def("feed", &PyPcmAccumulator::feed)
        .def("remainder_bytes", &PyPcmAccumulator::remainder_bytes)
        .def("remainder_samples", &PyPcmAccumulator::remainder_samples)
        .def("flush_bytes", &PyPcmAccumulator::flush_bytes);

    nb::class_<PyOpusFrameBuilder>(m, "OpusFrameBuilder")
        .def(nb::init<size_t>(), nb::arg("capacity") = Services::MIC_FRAME_MAX)
        .def("reset", &PyOpusFrameBuilder::reset)
        .def("add_packet", &PyOpusFrameBuilder::add_packet)
        .def("packet_count", &PyOpusFrameBuilder::packet_count)
        .def("size", &PyOpusFrameBuilder::size)
        .def("capacity", &PyOpusFrameBuilder::capacity)
        .def("empty", &PyOpusFrameBuilder::empty)
        .def("data", &PyOpusFrameBuilder::data);

    m.def("parse_opus_frame", [](nb::bytes frame) -> nb::object {
        std::vector<Services::OpusPacketView> packets;
        const auto* d = reinterpret_cast<const uint8_t*>(frame.c_str());
        bool ok = Services::parseOpusFrame(d, frame.size(), packets);
        if (!ok) return nb::none();
        nb::list res;
        for (const auto& p : packets) {
            res.append(nb::bytes(reinterpret_cast<const char*>(p.data), p.len));
        }
        return res;
    });

    m.def("parse_pcm_frame", [](nb::bytes frame) -> nb::object {
        const uint8_t* pcm = nullptr;
        size_t len = 0;
        const auto* d = reinterpret_cast<const uint8_t*>(frame.c_str());
        bool ok = Services::parsePcmFrame(d, frame.size(), pcm, len);
        if (!ok) return nb::none();
        return nb::bytes(reinterpret_cast<const char*>(pcm), len);
    });

    m.def("build_pcm_frame", [](nb::bytes pcm, size_t capacity) -> nb::object {
        std::vector<uint8_t> buf(capacity);
        const auto* d = reinterpret_cast<const uint8_t*>(pcm.c_str());
        size_t n = Services::buildPcmFrame(buf.data(), capacity, d, pcm.size());
        if (n == 0) return nb::none();
        return nb::bytes(reinterpret_cast<const char*>(buf.data()), n);
    }, nb::arg("pcm"), nb::arg("capacity") = Services::MIC_FRAME_MAX);
}
