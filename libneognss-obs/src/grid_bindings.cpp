// SPDX-License-Identifier: GPL-3.0-only
#include "grid_cnex.hpp"
#include <pybind11/numpy.h>
namespace py = pybind11;
namespace neognss_obs::python_bindings::grid {
using namespace neognss_obs;
auto lock(GridCnexProcessor &p) {
    std::unique_lock guard(p.mutex, std::try_to_lock);
    if (!guard.owns_lock())
        throw std::runtime_error("Concurrent SBAS processor use");
    return guard;
}
py::array intervals(std::vector<GridInterval> rows) {
    auto data = std::make_unique<std::vector<GridInterval>>(std::move(rows));
    auto ptr = data.get();
    py::capsule owner(ptr, [](void *p) {
        delete static_cast<std::vector<GridInterval> *>(p);
    });
    data.release();
    py::array out(py::dtype::of<GridInterval>(), {py::ssize_t(ptr->size())},
                  {py::ssize_t(sizeof(GridInterval))}, ptr->data(), owner);
    out.attr("setflags")(false);
    return out;
}
template <bool Events>
py::object feed(GridCnexProcessor &p, const py::object &batch) {
    auto capsules = batch.attr("__arrow_c_array__")().cast<py::tuple>();
    auto schema = static_cast<ArrowSchema *>(
        PyCapsule_GetPointer(capsules[0].ptr(), "arrow_schema"));
    auto array = static_cast<ArrowArray *>(
        PyCapsule_GetPointer(capsules[1].ptr(), "arrow_array"));
    if (!schema || !array)
        throw py::error_already_set();
    std::vector<GridInterval> rows;
    {
        py::gil_scoped_release release;
        auto guard = lock(p);
        if constexpr (Events)
            p.events(schema, array);
        else
            rows = p.feed(schema, array);
    }
    if constexpr (Events)
        return py::none();
    else
        return intervals(std::move(rows));
}
} // namespace neognss_obs::python_bindings::grid
void bind_grid(py::module_ &m) {
    using namespace neognss_obs;
    using namespace neognss_obs::python_bindings::grid;
    PYBIND11_NUMPY_DTYPE(GridInterval, start_gpst_ms, end_gpst_ms,
                         satellite_number, band, mask_bit, iodi, givei,
                         frame_id, stream_id, latitude, longitude, delay_m,
                         vtec_tecu);
    py::class_<GridCnexProcessor>(m, "GridCnexProcessor")
        .def(py::init<std::string, int64_t>())
        .def("begin_day",
             [](GridCnexProcessor &p, int64_t day) {
                 py::gil_scoped_release release;
                 auto guard = lock(p);
                 p.begin_day(day);
             })
        .def("events", &feed<true>)
        .def("feed", &feed<false>)
        .def("end_day",
             [](GridCnexProcessor &p) {
                 std::vector<GridInterval> rows;
                 {
                     py::gil_scoped_release release;
                     auto guard = lock(p);
                     rows = p.end_day();
                 }
                 return intervals(std::move(rows));
             })
        .def("finish",
             [](GridCnexProcessor &p) {
                 std::vector<GridInterval> rows;
                 {
                     py::gil_scoped_release release;
                     auto guard = lock(p);
                     rows = p.finish();
                 }
                 return intervals(std::move(rows));
             })
        .def_property_readonly("diagnostics", [](GridCnexProcessor &p) {
            auto guard = lock(p);
            return py::module_::import("json").attr("loads")(
                p.diagnostics().dump());
        });
}
