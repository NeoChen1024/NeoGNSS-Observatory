// SPDX-License-Identifier: GPL-3.0-only
#include "python_bindings.hpp"
#include <neognss_obs/broadcast_decoder.hpp>
#include <pybind11/stl.h>
namespace py = pybind11;
void bind_broadcast(py::module_ &m) {
    using neognss_obs::BroadcastMessageDecoder;
    using neognss_obs::python_bindings::ArrowImport;
    py::class_<BroadcastMessageDecoder>(m, "BroadcastMessageDecoder")
        .def(py::init<std::string, int64_t>(), py::arg("setup_id"),
             py::arg("snapshot_interval_s") = 3600)
        .def("feed",
             [](BroadcastMessageDecoder &d, const py::object &batch) {
                 ArrowImport input(batch);
                 py::gil_scoped_release release;
                 return d.feed(input.schema, input.array);
             })
        .def("discontinuity", &BroadcastMessageDecoder::discontinuity,
             py::call_guard<py::gil_scoped_release>())
        .def_property_readonly("diagnostics",
                               &BroadcastMessageDecoder::diagnostics);
}
