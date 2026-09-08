// SPDX-License-Identifier: GPL-3.0-only
#include <cppgnss/sbf.hpp>
#include <algorithm>
#include <mutex>
#include <neognss_obs/processing.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;
using neognss_obs::Json;
namespace {
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
    template <class... A> explicit Guarded(A &&...a) : value(std::forward<A>(a)...) {}
    template <class F> auto run(F &&f) {
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock())
            throw std::runtime_error("Concurrent use of the same processing state");
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
struct SbfBatch {
    explicit SbfBatch(std::vector<uint16_t> ids = {}) : block_ids(std::move(ids)) {}
    std::vector<uint16_t> block_ids;
    cppgnss::StreamDecoder reader{cppgnss::Protocol::sbf};
    std::vector<cppgnss::SBF::Block> feed(std::span<const uint8_t> data) {
        std::vector<cppgnss::SBF::Block> out;
        reader.feed(data, [&](const cppgnss::FrameView &f) {
            if (!block_ids.empty() && std::find(block_ids.begin(), block_ids.end(), f.id) == block_ids.end())
                return;
            out.push_back(cppgnss::SBF::decode(f.id, f.revision, f.payload));
            out.back().offset = f.offset;
        });
        return out;
    }
};
} // namespace
PYBIND11_MODULE(_native, m) {
    using Scan = Guarded<neognss_obs::DatasetScan>;
    py::class_<Scan>(m, "DatasetScan")
        .def(py::init<std::string, bool, double>(), py::arg("protocol") = "ubx", py::arg("qa") = true,
             py::arg("gap_timeout") = 50)
        .def("feed", [](Scan &s, py::buffer data) {
            auto info = data.request(); auto b = view(info);
            return run(s, [&](auto &p) { return p.feed(b); });
        })
        .def("finish", [](Scan &s) { return run(s, [](auto &p) { return p.finish(); }); })
        .def("summary", [](Scan &s) { return run(s, [](auto &p) { return p.summary(); }); });
    m.doc() = "Batched native GNSS processing; file/Parquet I/O belongs to Python.";
    m.attr("archive_schema") = "UBXIDX04-native-2-protocol-filter";
    m.def("archive_index", [](py::buffer data) {
        auto info = data.request();
        auto input = view(info);
        std::vector<uint8_t> result;
        {
            py::gil_scoped_release release;
            result = neognss_obs::archive_index(input);
        }
        return py::bytes(reinterpret_cast<const char *>(result.data()), result.size());
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
        .def("finish", [](Subframes &s) { return run(s, [](auto &p) { return p.finish(); }); })
        .def("summary", [](Subframes &s) { return run(s, [](auto &p) { return p.summary(); }); });
    using Clock = Guarded<neognss_obs::ClockProcessor>;
    py::class_<Clock>(m, "ClockProcessor")
        .def(py::init<double, double, double>(), py::arg("max_gap") = 50, py::arg("tolerance") = 50000,
             py::arg("temperature_max_age") = 5)
        .def("feed",
             [](Clock &s, py::buffer data, const std::string &source) {
                 auto info = data.request();
                 auto b = view(info);
                 return run(s, [&](auto &p) { return p.feed(b, source); });
             })
        .def("end_file", [](Clock &s) { return run(s, [](auto &p) { return p.end_file(); }); })
        .def("finish", [](Clock &s) { return run(s, [](auto &p) { return p.finish(); }); })
        .def("summary", [](Clock &s) { return run(s, [](auto &p) { return p.summary(); }); });
    using Grid = Guarded<neognss_obs::GridProcessor>;
    py::class_<Grid>(m, "GridProcessor")
        .def(py::init<double, double, double>(), py::arg("correction_age") = 600, py::arg("mask_age") = 1200,
             py::arg("gap_timeout") = 0)
        .def("process",
             [](Grid &s, py::object rows) {
                 auto batch = from_python(rows);
                 return run(s, [&](auto &p) { return p.process(batch); });
             })
        .def("process_frames", [](Grid &s, py::object rows) {
            auto batch = from_python(rows);
            return run(s, [&](auto &p) { return p.process_frames(batch); });
        })
        .def("finish", [](Grid &s, int64_t end) { return run(s, [&](auto &p) { return p.finish(end); }); })
        .def_property_readonly("diagnostics", [](Grid &s) { return run(s, [](auto &p) { return p.diagnostics(); }); });
    m.def("igp_coordinates", [] {
        py::dict out;
        for (unsigned b = 0; b <= 10; ++b)
            for (unsigned p = 1; p <= 201; ++p)
                if (auto c = cppgnss::SBAS::igp_coordinate(b, p))
                    out[py::make_tuple(b, p)] = py::make_tuple(c->first, c->second);
        return out;
    });
    using Sbf = Guarded<SbfBatch>;
    py::class_<Sbf>(m, "SbfParser")
        .def(py::init<std::vector<uint16_t>>(), py::arg("block_ids") = std::vector<uint16_t>{})
        .def("feed",
             [](Sbf &self, py::buffer data) {
                 auto info = data.request();
                 auto b = view(info);
                 std::vector<cppgnss::SBF::Block> blocks;
                 {
                     py::gil_scoped_release release;
                     blocks = self.run([&](auto &p) { return p.feed(b); });
                 }
                 py::list out;
                 for (const auto &block : blocks) {
                     py::dict row, fields;
                     row["id"] = block.id;
                     row["offset"] = py::cast(block.offset);
                     row["revision"] = block.revision;
                     row["name"] = block.name;
                     const char *names[] = {"decoded", "unknown_block", "unsupported_schema", "invalid_payload"};
                     row["status"] = names[int(block.status)];
                     row["error"] = block.error;
                     row["payload"] =
                         py::bytes(reinterpret_cast<const char *>(block.payload.data()), block.payload.size());
                     row["trailing"] =
                         py::bytes(reinterpret_cast<const char *>(block.trailing.data()), block.trailing.size());
                     for (auto &[name, value] : block.fields)
                         fields[py::str(name)] = std::visit(
                             [](const auto &v) -> py::object {
                                 if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::vector<uint8_t>>)
                                     return py::bytes(reinterpret_cast<const char *>(v.data()), v.size());
                                 else if constexpr (std::is_same_v<std::decay_t<decltype(v)>,
                                                                   cppgnss::SBF::WideInteger>) {
                                     return py::module_::import("builtins")
                                         .attr("int")
                                         .attr("from_bytes")(
                                             py::bytes(reinterpret_cast<const char *>(v.little_endian.data()),
                                                       v.little_endian.size()),
                                             "little", py::arg("signed") = v.is_signed);
                                 } else
                                     return py::cast(v);
                             },
                             value);
                     row["fields"] = fields;
                     out.append(row);
                     if (auto sbas = cppgnss::SBF::extract_sbas_l1(block)) {
                         row["sbas"] = to_python(neognss_obs::sbas_message(sbas->decoded));
                         row["constellation"] = "SBAS";
                         row["prn"] = sbas->prn;
                         row["signal"] = "L1CA";
                         // This is a native SBF GPST timestamp, not a UBX context estimate.
                         row["gpst_ms"] = (sbas->week != 65535 && sbas->tow_ms < 604800000)
                                              ? py::object(py::int_(int64_t(sbas->week) * 604800000 + sbas->tow_ms))
                                              : py::object(py::none());
                         row["receiver_crc_passed"] = sbas->receiver_crc_passed;
                     }
                 }
                 return out;
             })
        .def("finish",
             [](Sbf &self) {
                 self.run([](auto &p) {
                     p.reader.finish();
                     return 0;
                 });
             })
        .def("summary", [](Sbf &self) {
            return run(self, [](auto &p) -> Json {
                return {{"source_bytes", p.reader.bytes},
                        {"frames", p.reader.frames},
                        {"invalid", p.reader.invalid},
                        {"skipped_protocol_frames", p.reader.skipped_protocol_frames},
                        {"skipped_protocol_bytes", p.reader.skipped_protocol_bytes},
                        {"noise", p.reader.noise}};
            });
        });
    m.def("sbf_schemas", [] {
        py::list out;
        for (auto &s : cppgnss::SBF::schemas())
            out.append(py::make_tuple(s.id, s.name, !s.fields.empty()));
        return out;
    });
    using Planner = Guarded<neognss_obs::SegmentPlanner>;
    py::class_<Planner>(m, "SegmentPlanner")
        .def(py::init(
            [](py::object joins, int64_t timeout) { return std::make_unique<Planner>(from_python(joins), timeout); }))
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
        .def("finish", [](Planner &s) { return run(s, [](auto &p) { return p.finish(); }); });
}
