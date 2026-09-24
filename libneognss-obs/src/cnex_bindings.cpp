// SPDX-License-Identifier: GPL-3.0-only
#include <neognss_obs/cnex_engine.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
namespace py = pybind11;
namespace neognss_obs::python_bindings::cnex {
py::object to_python(const nlohmann::json &j) {
    return py::module_::import("json").attr("loads")(j.dump());
}
py::tuple export_array(neognss_obs::CnexBatch &batch, py::object requested) {
    if (!requested.is_none())
        throw py::value_error("Requested schema conversion is unsupported");
    if (!batch.array.release)
        throw py::value_error("Batch already consumed");
    // Build both owners before transferring the Arrow resources. Allocation
    // or capsule creation failures leave this batch intact.
    auto s = std::make_unique<ArrowSchema>();
    auto a = std::make_unique<ArrowArray>();
    py::capsule sc(s.get(), "arrow_schema", [](PyObject *c) {
        auto p =
            static_cast<ArrowSchema *>(PyCapsule_GetPointer(c, "arrow_schema"));
        if (p) {
            if (p->release)
                p->release(p);
            delete p;
        }
    });
    auto *schema_owner = s.release();
    py::capsule ac(a.get(), "arrow_array", [](PyObject *c) {
        auto p =
            static_cast<ArrowArray *>(PyCapsule_GetPointer(c, "arrow_array"));
        if (p) {
            if (p->release)
                p->release(p);
            delete p;
        }
    });
    auto *array_owner = a.release();
    *schema_owner = batch.schema;
    batch.schema.release = nullptr;
    *array_owner = batch.array;
    batch.array.release = nullptr;
    return py::make_tuple(sc, ac);
}
} // namespace neognss_obs::python_bindings::cnex
void bind_cnex(py::module_ &m) {
    using namespace neognss_obs::python_bindings::cnex;
    using Probe = neognss_obs::CnexTimeProbe;
    using Batch = neognss_obs::CnexBatch;
    using Reader = neognss_obs::CnexEngine;
    py::class_<Probe>(m, "CnexTimeProbe")
        .def(py::init<const std::string &, std::optional<int64_t>,
                      std::optional<uint16_t>>(),
             py::arg("protocol"), py::arg("rtcm_reference_gpst_s") = py::none(),
             py::arg("rtcm_station_id") = py::none())
        .def("feed",
             [](Probe &r, py::bytes bytes) {
                 char *p;
                 Py_ssize_t n;
                 if (PyBytes_AsStringAndSize(bytes.ptr(), &p, &n))
                     throw py::error_already_set();
                 py::gil_scoped_release release;
                 r.feed({reinterpret_cast<const uint8_t *>(p), size_t(n)});
             })
        .def("result", [](Probe &r) { return to_python(r.result()); });
    py::class_<Batch, std::shared_ptr<Batch>>(m, "CnexArrowBatch")
        .def("__arrow_c_array__", &export_array,
             py::arg("requested_schema") = py::none());
    py::class_<Reader>(m, "CnexObservationReader")
        .def(py::init<const std::string &, const std::string &, unsigned,
                      int64_t, int64_t, unsigned, std::optional<int64_t>,
                      std::optional<uint16_t>>(),
             py::arg("protocol"), py::arg("setup_id"), py::arg("antenna"),
             py::arg("period_seconds"), py::arg("period_ps"),
             py::arg("decode_workers") = 4,
             py::arg("rtcm_reference_gpst_s") = py::none(),
             py::arg("rtcm_station_id") = py::none())
        .def("feed",
             [](Reader &r, py::bytes bytes) {
                 char *p;
                 Py_ssize_t n;
                 if (PyBytes_AsStringAndSize(bytes.ptr(), &p, &n))
                     throw py::error_already_set();
                 py::gil_scoped_release release;
                 return r.feed(
                     {reinterpret_cast<const uint8_t *>(p), size_t(n)});
             })
        .def("finish_telemetry",
             [](Reader &r) {
                 py::gil_scoped_release release;
                 return r.finish_telemetry();
             })
        .def("summary", [](Reader &r) { return to_python(r.summary()); })
        .def("time_error", [](Reader &r) { return to_python(r.time_error()); })
        .def("checkpoint", [](Reader &r) { return to_python(r.checkpoint()); })
        .def("restore", [](Reader &r, py::dict state) {
            r.restore(nlohmann::json::parse(py::module_::import("json")
                                                .attr("dumps")(state)
                                                .cast<std::string>()));
        });
}
