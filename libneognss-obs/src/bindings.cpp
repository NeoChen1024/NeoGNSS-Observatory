// SPDX-License-Identifier: GPL-3.0-only
#include "python_bindings.hpp"
#include <algorithm>
#include <cppgnss/sbf.hpp>
#include <cppgnss/ubx_names.hpp>
#include <map>
#include <neognss_obs/processing.hpp>
#include <neognss_obs/ubx_archive.hpp>
#include <pybind11/stl.h>

namespace py = pybind11;
void bind_ppp(py::module_ &);
void bind_stec(py::module_ &);
void bind_grid(py::module_ &);
void bind_cnex(py::module_ &);
void bind_broadcast(py::module_ &);
using neognss_obs::Json;
namespace neognss_obs::python_bindings::core {
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
            auto &c = counts[f.id()];
            ++c.frames;
            c.bytes += f.wire.size();
            if (sbf)
                ++c.revisions[f.revision()];
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
    using namespace neognss_obs::python_bindings;
    using core::RxMessageRatio;
    bind_ppp(m);
    bind_stec(m);
    bind_grid(m);
    bind_cnex(m);
    bind_broadcast(m);
    using Ratio = Guarded<RxMessageRatio>;
    py::class_<Ratio>(m, "RxMessageRatio")
        .def(py::init<const std::string &>(), py::arg("protocol") = "ubx")
        .def("feed",
             [](Ratio &s, py::buffer data) {
                 auto info = data.request();
                 auto bytes = byte_view(info);
                 s.run([&](auto &p) { p.feed(bytes); });
             })
        .def("summary", [](Ratio &s) {
            return to_python(s.run([](auto &p) { return p.summary(); }));
        });
    using Scan = Guarded<neognss_obs::DatasetScan>;
    py::class_<Scan>(m, "DatasetScan")
        .def(py::init<std::string, double>(), py::arg("protocol") = "ubx",
             py::arg("gap_timeout") = 50)
        .def("feed",
             [](Scan &s, py::buffer data) {
                 auto info = data.request();
                 auto b = byte_view(info);
                 s.run([&](auto &p) { p.feed(b); });
             })
        .def("finish", [](Scan &s) { s.run([](auto &p) { p.finish(); }); })
        .def("summary", [](Scan &s) {
            return to_python(s.run([](auto &p) { return p.summary(); }));
        });
    m.doc() =
        "Batched native GNSS processing; file/Parquet I/O belongs to Python.";
    m.attr("archive_schema") = "UBXIDX04-native-3-timegps-range";
    m.attr("archive_header_size") = neognss_obs::archive_header_size;
    m.attr("archive_record_format") = neognss_obs::archive_record_format;
    m.def("archive_index", [](py::buffer data) {
        auto info = data.request();
        auto input = byte_view(info);
        std::vector<uint8_t> result;
        {
            py::gil_scoped_release release;
            result = neognss_obs::archive_index(input);
        }
        return py::bytes(reinterpret_cast<const char *>(result.data()),
                         result.size());
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
                 auto b = byte_view(info);
                 s.run([&](auto &p) { p.feed(description, b); });
             })
        .def("finish", [](Planner &s) {
            return to_python(s.run([](auto &p) { return p.finish(); }));
        });
}
