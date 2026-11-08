// SPDX-License-Identifier: GPL-3.0-only
#include "python_bindings.hpp"
#include <neognss_obs/ppp.hpp>
#include <pybind11/stl.h>
namespace py = pybind11;
using namespace neognss_obs;
void bind_ppp(py::module_ &m) {
    using namespace neognss_obs::python_bindings;
    using Reader = Guarded<ObservationReader>;
    using Solver = Guarded<PppFloat>;
    PYBIND11_NUMPY_DTYPE(PppEpoch, gpst_ns, status, satellites, x, y, z, qxx,
                         qyy, qzz, qxy, qyz, qzx, east, north, up, sigma_e,
                         sigma_n, sigma_u, clock_ns, clock_sigma_ns, ztd_m,
                         ztd_sigma_m);
    PYBIND11_NUMPY_DTYPE(PppSatellite, gpst_ns, prn, used, slip, azimuth_deg,
                         elevation_deg, phase_residual_m, code_residual_m);
    py::class_<ObservationBatch>(m, "ObservationBatch")
        .def_property_readonly("epochs_ns",
                               [](const ObservationBatch &b) {
                                   std::vector<int64_t> times;
                                   for (const auto &e : b.epochs)
                                       times.push_back(e.gpst_ns);
                                   return times;
                               })
        .def_property_readonly(
            "size", [](const ObservationBatch &b) { return b.epochs.size(); })
        .def_property_readonly("start_ns",
                               [](const ObservationBatch &b) {
                                   return b.epochs.empty()
                                              ? int64_t(0)
                                              : b.epochs.front().gpst_ns;
                               })
        .def_property_readonly("end_ns",
                               [](const ObservationBatch &b) {
                                   return b.epochs.empty()
                                              ? int64_t(0)
                                              : b.epochs.back().gpst_ns;
                               })
        .def("window",
             [](const ObservationBatch &b, int64_t start, int64_t end) {
                 ObservationBatch out;
                 for (const auto &e : b.epochs)
                     if (e.gpst_ns >= start && e.gpst_ns < end)
                         out.epochs.push_back(e);
                 return out;
             });
    py::class_<Reader>(m, "ObservationReader")
        .def(py::init<std::string>(), py::arg("protocol") = "ubx")
        .def("feed",
             [](Reader &s, py::buffer data) {
                 auto info = data.request();
                 auto bytes = byte_view(info);
                 return s.run([&](auto &r) { return r.feed(bytes); });
             })
        .def("finish", [](Reader &s) { s.run([](auto &r) { r.finish(); }); })
        .def("summary", [](Reader &s) {
            return to_python(s.run([](auto &r) { return r.summary(); }));
        });
    py::class_<Solver>(m, "PppFloat")
        .def(py::init([](py::dict settings) {
            auto j = from_python(settings);
            py::gil_scoped_release release;
            return std::make_unique<Solver>(j);
        }))
        .def("products",
             [](Solver &s, py::dict p) {
                 auto j = from_python(p);
                 s.run([&](auto &solver) { solver.products(j); });
             })
        .def("process",
             [](Solver &s, const ObservationBatch &b) {
                 auto r =
                     s.run([&](auto &solver) { return solver.process(b); });
                 return py::make_tuple(numpy_copy(r.epochs),
                                       numpy_copy(r.satellites));
             })
        .def("summary", [](Solver &s) {
            return to_python(
                s.run([](auto &solver) { return solver.summary(); }));
        });
}
