// SPDX-License-Identifier: GPL-3.0-only
#include <mutex>
#include <neognss_obs/ppp.hpp>
#include <pybind11/numpy.h>
#include <pybind11/stl.h>
namespace py = pybind11;
using namespace neognss_obs;
namespace {
Json json_arg(py::handle value) {
  return Json::parse(
      py::module_::import("json").attr("dumps")(value).cast<std::string>());
}
py::object json_out(const Json &v) {
  return py::module_::import("json").attr("loads")(v.dump());
}
template <class T> py::array_t<T> array(const std::vector<T> &v) {
  py::array_t<T> a(v.size());
  if (!v.empty())
    std::memcpy(a.mutable_data(), v.data(), v.size() * sizeof(T));
  return a;
}
struct Reader {
  ObservationReader value;
  std::mutex mutex;
  explicit Reader(const std::string &p) : value(p) {}
};
struct Solver {
  PppFloat value;
  std::mutex mutex;
  explicit Solver(const Json &s) : value(s) {}
};
template <class T> auto lock(T &s) {
  std::unique_lock guard(s.mutex, std::try_to_lock);
  if (!guard.owns_lock())
    throw std::runtime_error("Concurrent use of PPP processing state");
  return guard;
}
} // namespace
void bind_ppp(py::module_ &m) {
  PYBIND11_NUMPY_DTYPE(PppEpoch, gpst_ns, status, satellites, x, y, z, qxx, qyy,
                       qzz, qxy, qyz, qzx, east, north, up, sigma_e, sigma_n,
                       sigma_u, clock_ns, clock_sigma_ns, ztd_m, ztd_sigma_m);
  PYBIND11_NUMPY_DTYPE(PppSatellite, gpst_ns, prn, used, slip, azimuth_deg,
                       elevation_deg, phase_residual_m, code_residual_m);
  py::class_<ObservationBatch>(m, "ObservationBatch")
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
      .def("window", [](const ObservationBatch &b, int64_t start, int64_t end) {
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
             auto b = data.request();
             if (b.ndim != 1 || b.itemsize != 1 || b.strides[0] != 1 ||
                 !b.readonly)
               throw py::value_error("Expected read-only contiguous bytes");
             py::gil_scoped_release release;
             auto guard = lock(s);
             return s.value.feed(
                 {static_cast<const uint8_t *>(b.ptr), size_t(b.size)});
           })
      .def("finish",
           [](Reader &s) {
             py::gil_scoped_release release;
             auto guard = lock(s);
             s.value.finish();
           })
      .def("summary", [](Reader &s) {
        auto guard = lock(s);
        return json_out(s.value.summary());
      });
  py::class_<Solver>(m, "PppFloat")
      .def(py::init([](py::dict settings) {
        auto j = json_arg(settings);
        py::gil_scoped_release release;
        return std::make_unique<Solver>(j);
      }))
      .def("products",
           [](Solver &s, py::dict p) {
             auto j = json_arg(p);
             py::gil_scoped_release release;
             auto guard = lock(s);
             s.value.products(j);
           })
      .def("process",
           [](Solver &s, const ObservationBatch &b) {
             PppResult r;
             {
               py::gil_scoped_release release;
               auto guard = lock(s);
               r = s.value.process(b);
             }
             return py::make_tuple(array(r.epochs), array(r.satellites));
           })
      .def("summary", [](Solver &s) {
        auto guard = lock(s);
        return json_out(s.value.summary());
      });
}
