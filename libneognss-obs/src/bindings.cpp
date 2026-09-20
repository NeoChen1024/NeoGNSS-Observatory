// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <cppgnss/sbf.hpp>
#include <cppgnss/ubx_names.hpp>
#include <map>
#include <mutex>
#include <neognss_obs/processing.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;
void bind_ppp(py::module_ &);
void bind_stec(py::module_ &);
void bind_cnex(py::module_ &);
using neognss_obs::Json;
// pybind11/libc++ compares RTTI names: bound helpers need component-qualified
// identities, not same-named anonymous-namespace types in separate TUs.
namespace neognss_obs::python_bindings::core {
py::object to_python(const Json &value) {
    if (value.is_null())
        return py::none();
    if (value.is_boolean())
        return py::bool_(value.get<bool>());
    if (value.is_number_unsigned())
        return py::int_(value.get<uint64_t>());
    if (value.is_number_integer())
        return py::int_(value.get<int64_t>());
    if (value.is_number_float())
        return py::float_(value.get<double>());
    if (value.is_string())
        return py::str(value.get_ref<const std::string &>());
    if (value.is_array()) {
        py::list out;
        for (auto &v : value)
            out.append(to_python(v));
        return out;
    }
    py::dict out;
    for (auto it = value.begin(); it != value.end(); ++it)
        out[py::str(it.key())] = to_python(it.value());
    return out;
}
Json from_python(py::handle value) {
    if (py::isinstance<py::bytes>(value)) {
        const auto bytes = value.cast<std::string>();
        return Json::binary(std::vector<uint8_t>(bytes.begin(), bytes.end()));
    }
    if (value.is_none())
        return nullptr;
    if (py::isinstance<py::bool_>(value))
        return value.cast<bool>();
    if (py::isinstance<py::int_>(value))
        return value.cast<int64_t>();
    if (py::isinstance<py::float_>(value))
        return value.cast<double>();
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
    throw py::type_error("Expected numeric/string/list/dict batch values");
}
std::span<const uint8_t> view(const py::buffer_info &b) {
    if (b.ndim != 1 || b.itemsize != 1 || b.strides[0] != 1 || !b.readonly)
        throw py::value_error("Expected a contiguous read-only byte buffer");
    return {static_cast<const uint8_t *>(b.ptr), size_t(b.size)};
}
template <class T> struct Guarded {
    T value;
    std::mutex mutex;
    template <class... A>
    explicit Guarded(A &&...a) : value(std::forward<A>(a)...) {}
    template <class F> auto run(F &&f) {
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock())
            throw std::runtime_error(
                "Concurrent use of the same processing state");
        return f(value);
    }
};
template <class T, class F> py::object run(Guarded<T> &self, F &&f) {
    Json result;
    {
        py::gil_scoped_release release;
        result = self.run(std::forward<F>(f));
    }
    return to_python(result);
}
struct RxMessageRatio {
    bool sbf;
    cppgnss::StreamDecoder reader;
    explicit RxMessageRatio(const std::string &protocol)
        : sbf(protocol == "sbf"),
          reader(sbf ? cppgnss::Protocol::sbf : cppgnss::Protocol::ubx) {
        if (protocol != "ubx" && protocol != "sbf")
            throw std::invalid_argument("Expected ubx or sbf");
    }
    struct Counts {
        uint64_t frames = 0, bytes = 0;
        std::map<uint8_t, uint64_t> revisions;
    };
    std::map<uint16_t, Counts> counts;
    void feed(std::span<const uint8_t> data) {
        reader.feed(data, [&](const cppgnss::FrameView &f) {
            auto &c = counts[f.id];
            ++c.frames;
            c.bytes += f.wire.size();
            if (sbf)
                ++c.revisions[f.revision];
        });
    }
    Json summary() const {
        Json rows = Json::array();
        uint64_t valid_bytes = 0;
        for (const auto &[id, c] : counts) {
            std::string name = "Unknown";
            if (!sbf)
                name = UBX::ubx_msg_name(id >> 8, id & 255);
            else
                for (const auto &schema : cppgnss::SBF::schemas())
                    if (schema.id == id) {
                        name = schema.name;
                        break;
                    }
            Json revisions = Json::array();
            for (const auto &[revision, count] : c.revisions)
                revisions.push_back(revision);
            rows.push_back({{"id", id},
                            {"name", name},
                            {"frames", c.frames},
                            {"bytes", c.bytes},
                            {"revisions", revisions}});
            valid_bytes += c.bytes;
        }
        return {{"messages", rows},
                {"source_bytes", reader.bytes},
                {"valid_bytes", valid_bytes},
                {"invalid_candidates", reader.invalid},
                {"unclassified_bytes",
                 reader.bytes - valid_bytes - reader.skipped_protocol_bytes},
                {"pending_tail_bytes", reader.pending_bytes()},
                {"foreign_frames", reader.skipped_protocol_frames},
                {"foreign_bytes", reader.skipped_protocol_bytes}};
    }
};
} // namespace neognss_obs::python_bindings::core
PYBIND11_MODULE(_native, m) {
    using namespace neognss_obs::python_bindings::core;
    bind_ppp(m);
    bind_stec(m);
    bind_cnex(m);
    using Ratio = Guarded<RxMessageRatio>;
    py::class_<Ratio>(m, "RxMessageRatio")
        .def(py::init<const std::string &>(), py::arg("protocol") = "ubx")
        .def("feed",
             [](Ratio &s, py::buffer data) {
                 auto info = data.request();
                 auto bytes = view(info);
                 py::gil_scoped_release release;
                 s.run([&](auto &p) { p.feed(bytes); });
             })
        .def("summary", [](Ratio &s) {
            return run(s, [](auto &p) { return p.summary(); });
        });
    using Scan = Guarded<neognss_obs::DatasetScan>;
    py::class_<Scan>(m, "DatasetScan")
        .def(py::init<std::string, bool, double>(), py::arg("protocol") = "ubx",
             py::arg("qa") = true, py::arg("gap_timeout") = 50)
        .def("feed",
             [](Scan &s, py::buffer data) {
                 auto info = data.request();
                 auto b = view(info);
                 return run(s, [&](auto &p) { return p.feed(b); });
             })
        .def("finish",
             [](Scan &s) { return run(s, [](auto &p) { return p.finish(); }); })
        .def("summary", [](Scan &s) {
            return run(s, [](auto &p) { return p.summary(); });
        });
    m.doc() =
        "Batched native GNSS processing; file/Parquet I/O belongs to Python.";
    m.attr("archive_schema") = "UBXIDX04-native-2-protocol-filter";
    m.def("archive_index", [](py::buffer data) {
        auto info = data.request();
        auto input = view(info);
        std::vector<uint8_t> result;
        {
            py::gil_scoped_release release;
            result = neognss_obs::archive_index(input);
        }
        return py::bytes(reinterpret_cast<const char *>(result.data()),
                         result.size());
    });
    using Subframes = Guarded<neognss_obs::SubframeProcessor>;
    py::class_<Subframes>(m, "SubframeProcessor")
        .def(py::init<bool>(), py::arg("sbas_only") = true)
        .def("feed",
             [](Subframes &s, py::buffer data) {
                 auto info = data.request();
                 auto b = view(info);
                 return run(s, [&](auto &p) { return p.feed(b); });
             })
        .def("finish",
             [](Subframes &s) {
                 return run(s, [](auto &p) { return p.finish(); });
             })
        .def("summary", [](Subframes &s) {
            return run(s, [](auto &p) { return p.summary(); });
        });
    using Grid = Guarded<neognss_obs::GridProcessor>;
    py::class_<Grid>(m, "GridProcessor")
        .def(py::init<double, double, double>(),
             py::arg("correction_age") = 600, py::arg("mask_age") = 1200,
             py::arg("gap_timeout") = 0)
        .def("process",
             [](Grid &s, py::object rows) {
                 auto batch = from_python(rows);
                 return run(s, [&](auto &p) { return p.process(batch); });
             })
        .def("process_frames",
             [](Grid &s, py::object rows) {
                 auto batch = from_python(rows);
                 return run(s,
                            [&](auto &p) { return p.process_frames(batch); });
             })
        .def("finish",
             [](Grid &s, int64_t end) {
                 return run(s, [&](auto &p) { return p.finish(end); });
             })
        .def_property_readonly("diagnostics", [](Grid &s) {
            return run(s, [](auto &p) { return p.diagnostics(); });
        });
    m.def("igp_coordinates", [] {
        py::dict out;
        for (unsigned b = 0; b <= 10; ++b)
            for (unsigned p = 1; p <= 201; ++p)
                if (auto c = cppgnss::SBAS::igp_coordinate(b, p))
                    out[py::make_tuple(b, p)] =
                        py::make_tuple(c->first, c->second);
        return out;
    });
    m.def("sbf_schemas", [] {
        py::list out;
        for (auto &s : cppgnss::SBF::schemas())
            out.append(py::make_tuple(s.id, s.name, !s.fields.empty()));
        return out;
    });
    using Planner = Guarded<neognss_obs::SegmentPlanner>;
    py::class_<Planner>(m, "SegmentPlanner")
        .def(py::init([](py::object joins, int64_t timeout) {
            return std::make_unique<Planner>(from_python(joins), timeout);
        }))
        .def("feed",
             [](Planner &s, py::object source, py::buffer data) {
                 auto description = from_python(source);
                 auto info = data.request();
                 auto b = view(info);
                 py::gil_scoped_release release;
                 s.run([&](auto &p) {
                     p.feed(description, b);
                     return 0;
                 });
             })
        .def("finish", [](Planner &s) {
            return run(s, [](auto &p) { return p.finish(); });
        });
}
