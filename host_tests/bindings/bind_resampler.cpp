#include "bindings.h"
#include "audio_core/Resampler.h"
#include <nanobind/stl/vector.h>

void init_resampler(nb::module_& m) {
    nb::class_<LinearResampler>(m, "LinearResampler")
        .def(nb::init<>())
        .def("resample", [](LinearResampler& self, const std::vector<int16_t>& src,
                             size_t dst_frames, int channels) {
            size_t src_frames = channels > 0 ? src.size() / static_cast<size_t>(channels) : 0;
            std::vector<int16_t> dst(dst_frames * static_cast<size_t>(channels));
            self.resample(src.data(), src_frames, dst.data(), dst_frames, channels);
            return dst;
        }, nb::arg("src"), nb::arg("dst_frames"), nb::arg("channels") = 1);

    m.def("compute_resampled_frames", &computeResampledFrames,
          nb::arg("src_frames"), nb::arg("src_rate"), nb::arg("dst_rate"));
}
