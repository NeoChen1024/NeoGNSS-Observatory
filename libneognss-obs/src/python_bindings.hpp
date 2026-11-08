// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Shared pybind11 boundary mechanisms of the _native module's binding units.
#include <climits>
#include <cmath>
#include <cstring>
#include <mutex>
#include <nanoarrow/nanoarrow.h>
#include <neognss_obs/processing.hpp>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <span>
#include <stdexcept>
#include <vector>

// pybind11/libc++ compares RTTI names: bound helpers need component-qualified
// identities, not same-named anonymous-namespace types in separate TUs.
namespace neognss_obs::python_bindings {
namespace py = pybind11;

// Native Json values are JSON: a non-finite number has no JSON value. Output
// writes it as None; input rejects it.
inline py::object to_python(const Json &value) {
    if (value.is_null())
        return py::none();
    if (value.is_boolean())
        return py::bool_(value.get<bool>());
    if (value.is_number_unsigned())
        return py::int_(value.get<uint64_t>());
    if (value.is_number_integer())
        return py::int_(value.get<int64_t>());
    if (value.is_number_float()) {
        const auto x = value.get<double>();
        return std::isfinite(x) ? py::object(py::float_(x)) : py::none();
    }
    if (value.is_string())
        return py::str(value.get_ref<const std::string &>());
    if (value.is_array()) {
        py::list out;
        for (const auto &v : value)
            out.append(to_python(v));
        return out;
    }
    py::dict out;
    for (auto it = value.begin(); it != value.end(); ++it)
        out[py::str(it.key())] = to_python(it.value());
    return out;
}
inline Json from_python(py::handle value) {
    if (value.is_none())
        return nullptr;
    if (py::isinstance<py::bool_>(value))
        return value.cast<bool>();
    if (py::isinstance<py::int_>(value)) {
        int overflow = 0;
        const auto x = PyLong_AsLongLongAndOverflow(value.ptr(), &overflow);
        if (!overflow && !(x == -1 && PyErr_Occurred()))
            return int64_t(x);
        PyErr_Clear();
        if (overflow > 0) {
            const auto u = PyLong_AsUnsignedLongLong(value.ptr());
            if (!(u == ULLONG_MAX && PyErr_Occurred()))
                return uint64_t(u);
            PyErr_Clear();
        }
        // Like a JSON integer literal beyond 64 bits.
        return value.cast<double>();
    }
    if (py::isinstance<py::float_>(value)) {
        const auto x = value.cast<double>();
        if (!std::isfinite(x))
            throw py::value_error("Non-finite number is not a JSON value");
        return x;
    }
    if (py::isinstance<py::str>(value))
        return value.cast<std::string>();
    if (py::isinstance<py::dict>(value)) {
        Json out = Json::object();
        for (auto pair : py::reinterpret_borrow<py::dict>(value))
            out[pair.first.cast<std::string>()] = from_python(pair.second);
        return out;
    }
    if (py::isinstance<py::list>(value) || py::isinstance<py::tuple>(value)) {
        Json out = Json::array();
        for (auto item : py::reinterpret_borrow<py::iterable>(value))
            out.push_back(from_python(item));
        return out;
    }
    throw py::type_error("Expected a JSON-compatible value");
}

inline std::span<const uint8_t> byte_view(const py::buffer_info &b) {
    if (b.ndim != 1 || b.itemsize != 1 || b.strides[0] != 1 || !b.readonly)
        throw py::value_error("Expected a contiguous read-only byte buffer");
    return {static_cast<const uint8_t *>(b.ptr), size_t(b.size)};
}

// One Arrow C data interface export of a Python RecordBatch. The capsules
// keep the exported schema/array alive while native code reads them.
struct ArrowImport {
    py::tuple capsules;
    ArrowSchema *schema;
    ArrowArray *array;
    explicit ArrowImport(const py::object &batch)
        : capsules(batch.attr("__arrow_c_array__")().cast<py::tuple>()),
          schema(static_cast<ArrowSchema *>(
              PyCapsule_GetPointer(capsules[0].ptr(), "arrow_schema"))),
          array(static_cast<ArrowArray *>(
              PyCapsule_GetPointer(capsules[1].ptr(), "arrow_array"))) {
        if (!schema || !array)
            throw py::error_already_set();
    }
};

template <class T> py::array_t<T> numpy_copy(const std::vector<T> &v) {
    py::array_t<T> a(v.size());
    if (!v.empty())
        std::memcpy(a.mutable_data(), v.data(), v.size() * sizeof(T));
    return a;
}

// Python-owned native state. run() executes F without the GIL; concurrent use
// from another Python thread is an error rather than a wait.
template <class T> struct Guarded {
    T value;
    std::mutex mutex;
    template <class... A>
    explicit Guarded(A &&...a) : value(std::forward<A>(a)...) {}
    template <class F> auto run(F &&f) {
        py::gil_scoped_release release;
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock())
            throw std::runtime_error(
                "Concurrent use of the same processing state");
        return f(value);
    }
};
} // namespace neognss_obs::python_bindings
