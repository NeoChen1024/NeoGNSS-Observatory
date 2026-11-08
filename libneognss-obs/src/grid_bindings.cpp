// SPDX-License-Identifier: GPL-3.0-only
#include "python_bindings.hpp"
#include <neognss_obs/sbas_grid.hpp>
#include <pybind11/stl.h>
namespace py = pybind11;
void bind_grid(py::module_ &m) {
    using neognss_obs::SbasGridProcessor;
    using neognss_obs::python_bindings::ArrowImport;
    py::class_<SbasGridProcessor>(m, "SbasGridProcessor")
        .def(py::init<std::string, int64_t, int64_t, int64_t>(),
             py::arg("setup_id"), py::arg("interval_s") = 3600,
             py::arg("correction_age_s") = 600, py::arg("mask_age_s") = 1200)
        .def("feed",
             [](SbasGridProcessor &p, const py::object &batch) {
                 ArrowImport input(batch);
                 py::gil_scoped_release release;
                 return p.feed(input.schema, input.array);
             })
        .def("advance", &SbasGridProcessor::advance, py::arg("gpst_seconds"),
             py::arg("fraction_ps") = 0,
             py::call_guard<py::gil_scoped_release>())
        .def("discontinuity", &SbasGridProcessor::discontinuity,
             py::call_guard<py::gil_scoped_release>())
        .def_property_readonly("diagnostics", &SbasGridProcessor::diagnostics);
}
