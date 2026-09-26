// SPDX-License-Identifier: GPL-3.0-only
#include <neognss_obs/sbas_grid.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
namespace py = pybind11;
void bind_grid(py::module_ &m) {
    using neognss_obs::SbasGridProcessor;
    py::class_<SbasGridProcessor>(m, "SbasGridProcessor")
        .def(py::init<std::string, int64_t, int64_t, int64_t>(),
             py::arg("setup_id"), py::arg("interval_s") = 3600,
             py::arg("correction_age_s") = 600, py::arg("mask_age_s") = 1200)
        .def("feed",
             [](SbasGridProcessor &p, const py::object &batch) {
                 auto pair =
                     batch.attr("__arrow_c_array__")().cast<py::tuple>();
                 auto s = static_cast<ArrowSchema *>(
                     PyCapsule_GetPointer(pair[0].ptr(), "arrow_schema"));
                 auto a = static_cast<ArrowArray *>(
                     PyCapsule_GetPointer(pair[1].ptr(), "arrow_array"));
                 if (!s || !a)
                     throw py::error_already_set();
                 py::gil_scoped_release release;
                 return p.feed(s, a);
             })
        .def("advance", &SbasGridProcessor::advance, py::arg("gpst_seconds"),
             py::arg("fraction_ps") = 0,
             py::call_guard<py::gil_scoped_release>())
        .def("discontinuity", &SbasGridProcessor::discontinuity,
             py::call_guard<py::gil_scoped_release>())
        .def_property_readonly("diagnostics", &SbasGridProcessor::diagnostics);
}
