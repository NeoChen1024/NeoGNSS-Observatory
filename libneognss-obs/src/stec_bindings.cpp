// SPDX-License-Identifier: GPL-3.0-only
#include "python_bindings.hpp"
#include "stec_cnex.hpp"
#include <neognss_obs/broadcast_navigation.hpp>
#include <neognss_obs/stec.hpp>
#include <pybind11/stl.h>
namespace py = pybind11;
using namespace neognss_obs;
void bind_stec(py::module_ &m) {
    using namespace neognss_obs::python_bindings;
    using CnexReader = Guarded<StecCnexReader>;
    using Processor = Guarded<StecProcessor>;
    py::class_<BroadcastNavigation, std::shared_ptr<BroadcastNavigation>>(
        m, "BroadcastNavigation")
        .def(py::init<std::string>())
        .def("feed",
             [](BroadcastNavigation &s, const py::object &batch) {
                 ArrowImport input(batch);
                 py::gil_scoped_release release;
                 s.feed(input.schema, input.array);
             })
        .def("clear", &BroadcastNavigation::clear,
             py::call_guard<py::gil_scoped_release>())
        .def_property_readonly("decoded", &BroadcastNavigation::decoded)
        .def_property_readonly("decoded_by_family",
                               &BroadcastNavigation::decoded_by_family)
        .def("ecef", [](BroadcastNavigation &s,
                        const std::vector<int64_t> &times,
                        const std::vector<std::string> &systems,
                        const std::vector<int> &numbers) {
            if (times.size() != systems.size() ||
                times.size() != numbers.size())
                throw std::invalid_argument(
                    "Navigation query column lengths differ");
            py::array_t<double> result(
                {py::ssize_t(times.size()), py::ssize_t(3)});
            auto out = result.mutable_data();
            py::gil_scoped_release release;
            for (size_t i = 0; i < times.size(); ++i) {
                if (times[i] < 0 || systems[i].size() != 1)
                    throw std::invalid_argument(
                        "Invalid navigation query identity/time");
                if (!s.ecef(systems[i][0], numbers[i], times[i], out + i * 3))
                    std::fill(out + i * 3, out + i * 3 + 3,
                              std::numeric_limits<double>::quiet_NaN());
            }
            return result;
        });
    py::class_<CnexReader>(m, "StecCnexReader")
        .def(py::init([](std::string setup, py::list pairs) {
            return std::make_unique<CnexReader>(std::move(setup),
                                                from_python(pairs));
        }))
        .def("feed",
             [](CnexReader &s, const py::object &batch) {
                 ArrowImport input(batch);
                 return s.run([&](auto &r) {
                     return r.feed(input.schema, input.array);
                 });
             })
        .def("flush",
             [](CnexReader &s) {
                 return s.run([](auto &r) { return r.flush(); });
             })
        .def("complete",
             [](CnexReader &s, int64_t through_ns) {
                 return s.run([&](auto &r) { return r.complete(through_ns); });
             })
        .def("summary", [](CnexReader &s) {
            return to_python(s.run([](auto &r) { return r.summary(); }));
        });
    PYBIND11_NUMPY_DTYPE(
        StecSample, gpst_ns, arc_id, receiver_segment_start_ns, prn, system,
        pair_id, product_issues, raw_phase_gf_m, relative_stec_tecu, phase_gf_m,
        code_gf_corrected_m, elevation_deg, azimuth_deg, ipp_latitude_deg,
        ipp_longitude_deg, mapping, gim_stec_tecu, gim_rms_tecu);
    PYBIND11_NUMPY_DTYPE(StecArc, gpst_ns, end_ns, arc_id, samples,
                         leveling_samples, receiver_segment_start_ns, prn,
                         system, pair_id, valid, start_reason, end_reason,
                         provisional, level_offset_m, scatter_m);
    PYBIND11_NUMPY_DTYPE(DcbSample, gpst_ns, arc_id, prn, residual_tecu,
                         elevation_deg, azimuth_deg, gim_rms_tecu);
    m.attr("dcb_sample_dtype") = py::dtype::of<DcbSample>();
    py::class_<Processor>(m, "StecProcessor")
        .def(py::init([](py::dict settings) {
            auto s = from_python(settings);
            py::gil_scoped_release release;
            return std::make_unique<Processor>(s);
        }))
        .def("navigation",
             [](Processor &s, std::shared_ptr<BroadcastNavigation> navigation) {
                 s.run([&](auto &p) { p.navigation(std::move(navigation)); });
             })
        .def("products",
             [](Processor &s, py::dict products) {
                 auto j = from_python(products);
                 s.run([&](auto &p) { p.products(j); });
             })
        .def("restarts",
             [](Processor &s, const std::vector<int64_t> &boundaries) {
                 s.run([&](auto &p) { p.restarts(boundaries); });
             })
        .def("process",
             [](Processor &s, const ObservationBatch &b) {
                 auto r = s.run([&](auto &p) { return p.process(b); });
                 return py::make_tuple(numpy_copy(r.samples),
                                       numpy_copy(r.arcs));
             })
        .def("finish",
             [](Processor &s) {
                 return numpy_copy(s.run([](auto &p) { return p.finish(); }));
             })
        .def("summary",
             [](Processor &s) {
                 return to_python(s.run([](auto &p) { return p.summary(); }));
             })
        .def("preview",
             [](Processor &s) {
                 return numpy_copy(s.run([](auto &p) { return p.preview(); }));
             })
        .def("checkpoint",
             [](Processor &s) {
                 return to_python(
                     s.run([](auto &p) { return p.checkpoint(); }));
             })
        .def("restore", [](Processor &s, py::dict state) {
            auto j = from_python(state);
            s.run([&](auto &p) { p.restore(j); });
        });
    py::class_<ReceiverDcbFit>(m, "ReceiverDcbFit")
        .def(py::init([](py::dict settings) {
                 return std::make_unique<ReceiverDcbFit>(from_python(settings));
             }),
             py::arg("settings"))
        .def(
            "fit",
            [](const ReceiverDcbFit &f,
               py::array_t<DcbSample, py::array::c_style> rows,
               double meters_per_tecu) {
                if (rows.ndim() != 1)
                    throw py::value_error(
                        "Expected a one-dimensional DCB batch");
                // Snapshot the caller's array before releasing the GIL.
                std::vector<DcbSample> owned(rows.data(),
                                             rows.data() + rows.size());
                Json out;
                {
                    py::gil_scoped_release release;
                    out = f.fit(owned, meters_per_tecu);
                }
                return to_python(out);
            },
            py::arg("rows"), py::arg("meters_per_tecu"));
}
