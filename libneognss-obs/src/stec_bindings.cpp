// SPDX-License-Identifier: GPL-3.0-only
#include "stec_cnex.hpp"
#include <mutex>
#include <neognss_obs/broadcast_navigation.hpp>
#include <neognss_obs/stec.hpp>
#include <pybind11/numpy.h>
#include <pybind11/stl.h>
namespace py = pybind11;
using namespace neognss_obs;
// pybind11/libc++ compares RTTI names: bound helpers need component-qualified
// identities, not same-named anonymous-namespace types in separate TUs.
namespace neognss_obs::python_bindings::stec {
Json arg(py::handle x) {
    return Json::parse(
        py::module_::import("json").attr("dumps")(x).cast<std::string>());
}
py::object result(const Json &x) {
    return py::module_::import("json").attr("loads")(x.dump());
}
template <class T> py::array_t<T> array(const std::vector<T> &v) {
    py::array_t<T> a(v.size());
    if (!v.empty())
        std::memcpy(a.mutable_data(), v.data(), v.size() * sizeof(T));
    return a;
}
struct Processor {
    StecProcessor value;
    std::mutex mutex;
    explicit Processor(const Json &s) : value(s) {}
};
auto lock(Processor &s) {
    std::unique_lock guard(s.mutex, std::try_to_lock);
    if (!guard.owns_lock())
        throw std::runtime_error("Concurrent use of STEC state");
    return guard;
}
} // namespace neognss_obs::python_bindings::stec
void bind_stec(py::module_ &m) {
    using namespace neognss_obs::python_bindings::stec;
    py::class_<BroadcastNavigation, std::shared_ptr<BroadcastNavigation>>(
        m, "BroadcastNavigation")
        .def(py::init<std::string>())
        .def("feed",
             [](BroadcastNavigation &s, const py::object &batch) {
                 auto capsules =
                     batch.attr("__arrow_c_array__")().cast<py::tuple>();
                 auto schema = static_cast<ArrowSchema *>(
                     PyCapsule_GetPointer(capsules[0].ptr(), "arrow_schema"));
                 auto array = static_cast<ArrowArray *>(
                     PyCapsule_GetPointer(capsules[1].ptr(), "arrow_array"));
                 if (!schema || !array)
                     throw py::error_already_set();
                 py::gil_scoped_release release;
                 s.feed(schema, array);
             })
        .def("clear", &BroadcastNavigation::clear,
             py::call_guard<py::gil_scoped_release>())
        .def_property_readonly("decoded", &BroadcastNavigation::decoded)
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
    py::class_<StecCnexReader>(m, "StecCnexReader")
        .def(py::init([](std::string setup, py::list pairs) {
            return std::make_unique<StecCnexReader>(std::move(setup),
                                                    arg(pairs));
        }))
        .def("feed",
             [](StecCnexReader &s, const py::object &batch) {
                 auto capsules =
                     batch.attr("__arrow_c_array__")().cast<py::tuple>();
                 auto schema = static_cast<ArrowSchema *>(
                     PyCapsule_GetPointer(capsules[0].ptr(), "arrow_schema"));
                 auto array = static_cast<ArrowArray *>(
                     PyCapsule_GetPointer(capsules[1].ptr(), "arrow_array"));
                 if (!schema || !array)
                     throw py::error_already_set();
                 py::gil_scoped_release release;
                 std::unique_lock guard(s.mutex, std::try_to_lock);
                 if (!guard.owns_lock())
                     throw std::runtime_error(
                         "Concurrent CommonNEX reader use");
                 return s.feed(schema, array);
             })
        .def("flush",
             [](StecCnexReader &s) {
                 auto guard = std::unique_lock(s.mutex, std::try_to_lock);
                 if (!guard.owns_lock())
                     throw std::runtime_error(
                         "Concurrent CommonNEX reader use");
                 return s.flush();
             })
        .def("complete",
             [](StecCnexReader &s, int64_t through_ns) {
                 std::unique_lock guard(s.mutex, std::try_to_lock);
                 if (!guard.owns_lock())
                     throw std::runtime_error(
                         "Concurrent CommonNEX reader use");
                 return s.complete(through_ns);
             })
        .def("summary", [](StecCnexReader &s) {
            auto guard = std::unique_lock(s.mutex, std::try_to_lock);
            if (!guard.owns_lock())
                throw std::runtime_error("Concurrent CommonNEX reader use");
            return result(s.summary());
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
            auto s = arg(settings);
            py::gil_scoped_release release;
            return std::make_unique<Processor>(s);
        }))
        .def("navigation",
             [](Processor &s, std::shared_ptr<BroadcastNavigation> navigation) {
                 py::gil_scoped_release release;
                 auto guard = lock(s);
                 s.value.navigation(std::move(navigation));
             })
        .def("products",
             [](Processor &s, py::dict p) {
                 auto j = arg(p);
                 py::gil_scoped_release release;
                 auto guard = lock(s);
                 s.value.products(j);
             })
        .def("restarts",
             [](Processor &s, const std::vector<int64_t> &boundaries) {
                 py::gil_scoped_release release;
                 auto guard = lock(s);
                 s.value.restarts(boundaries);
             })
        .def("process",
             [](Processor &s, const ObservationBatch &b) {
                 StecResult r;
                 {
                     py::gil_scoped_release release;
                     auto guard = lock(s);
                     r = s.value.process(b);
                 }
                 return py::make_tuple(array(r.samples), array(r.arcs));
             })
        .def("finish",
             [](Processor &s) {
                 std::vector<StecArc> r;
                 {
                     py::gil_scoped_release release;
                     auto guard = lock(s);
                     r = s.value.finish();
                 }
                 return array(r);
             })
        .def("summary",
             [](Processor &s) {
                 auto guard = lock(s);
                 return result(s.value.summary());
             })
        .def("preview",
             [](Processor &s) {
                 std::vector<StecArc> out;
                 {
                     py::gil_scoped_release release;
                     auto guard = lock(s);
                     out = s.value.preview();
                 }
                 return array(out);
             })
        .def("checkpoint",
             [](Processor &s) {
                 Json j;
                 {
                     py::gil_scoped_release release;
                     auto guard = lock(s);
                     j = s.value.checkpoint();
                 }
                 return result(j);
             })
        .def("restore", [](Processor &s, py::dict state) {
            auto j = arg(state);
            py::gil_scoped_release release;
            auto guard = lock(s);
            s.value.restore(j);
        });
    m.def(
        "fit_receiver_dcb",
        [](py::array_t<DcbSample, py::array::c_style> rows, py::dict settings) {
            if (rows.ndim() != 1)
                throw py::value_error("Expected a one-dimensional DCB batch");
            // Snapshot the caller's array before releasing the GIL.
            std::vector<DcbSample> owned(rows.data(),
                                         rows.data() + rows.size());
            auto s = arg(settings);
            Json out;
            {
                py::gil_scoped_release release;
                out = fit_receiver_dcb(owned, s);
            }
            return result(out);
        });
}
