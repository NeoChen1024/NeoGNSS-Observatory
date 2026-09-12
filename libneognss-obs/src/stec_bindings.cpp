// SPDX-License-Identifier: GPL-3.0-only
#include <mutex>
#include <neognss_obs/stec.hpp>
#include <pybind11/numpy.h>
#include <pybind11/stl.h>
namespace py = pybind11;
using namespace neognss_obs;
namespace {
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
} // namespace
void bind_stec(py::module_ &m) {
  PYBIND11_NUMPY_DTYPE(StecSample, gpst_ns, arc_id, prn, product_issues,
                       phase_gf_m, code_gf_corrected_m, elevation_deg,
                       azimuth_deg, ipp_latitude_deg, ipp_longitude_deg,
                       mapping, gim_stec_tecu, gim_rms_tecu);
  PYBIND11_NUMPY_DTYPE(StecArc, gpst_ns, end_ns, arc_id, samples,
                       leveling_samples, prn, valid, start_reason, end_reason,
                       level_offset_m, scatter_m);
  PYBIND11_NUMPY_DTYPE(DcbSample, gpst_ns, arc_id, prn, residual_tecu,
                       elevation_deg, azimuth_deg, gim_rms_tecu);
  m.attr("dcb_sample_dtype") = py::dtype::of<DcbSample>();
  py::class_<Processor>(m, "StecProcessor")
      .def(py::init([](py::dict settings) {
        auto s = arg(settings);
        py::gil_scoped_release release;
        return std::make_unique<Processor>(s);
      }))
      .def("products",
           [](Processor &s, py::dict p) {
             auto j = arg(p);
             py::gil_scoped_release release;
             auto guard = lock(s);
             s.value.products(j);
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
      .def("summary", [](Processor &s) {
        auto guard = lock(s);
        return result(s.value.summary());
      });
  m.def("fit_receiver_dcb",
        [](py::array_t<DcbSample, py::array::c_style> rows, py::dict settings) {
          if (rows.ndim() != 1)
            throw py::value_error("Expected a one-dimensional DCB batch");
          // Snapshot the caller's array before releasing the GIL.
          std::vector<DcbSample> owned(rows.data(), rows.data() + rows.size());
          auto s = arg(settings);
          Json out;
          {
            py::gil_scoped_release release;
            out = fit_receiver_dcb(owned, s);
          }
          return result(out);
        });
}
