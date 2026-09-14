// SPDX-License-Identifier: GPL-3.0-only
#include <bit>
#include <boost/int128/int128.hpp>
#include <cppgnss/measurements.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <nanoarrow/nanoarrow.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <tuple>

namespace py = pybind11;
namespace {
using Tick = boost::int128::int128;
constexpr int64_t ps = 1000000000000LL;
void check(int result) {
    if (result)
        throw std::runtime_error("Arrow construction failed: " +
                                 std::to_string(result));
}
Tick time_of(const cppgnss::Measurements &e) {
    if (e.week == 65535 || !std::isfinite(e.tow_seconds) || e.tow_seconds < 0 ||
        e.tow_seconds >= 604800)
        throw std::runtime_error("Invalid measurement week/TOW");
    Tick tow = 0;
    if (e.tow_ms)
        tow = Tick(*e.tow_ms) * 1000000000;
    else {
        // Exact binary64 rational -> integer picoseconds, ties to even.
        auto bits = std::bit_cast<uint64_t>(e.tow_seconds);
        int exponent = int((bits >> 52) & 2047);
        uint64_t mantissa = bits & ((uint64_t(1) << 52) - 1);
        if (exponent)
            mantissa |= uint64_t(1) << 52;
        int shift = (exponent ? exponent - 1023 : -1022) - 52;
        Tick n = Tick(mantissa) * ps;
        if (shift >= 0)
            tow = n << shift;
        else if (-shift < 127) {
            int k = -shift;
            tow = n >> k;
            Tick rem = n - (tow << k), half = Tick(1) << (k - 1);
            if (rem > half || (rem == half && (tow & 1) != 0))
                ++tow;
        }
    }
    return Tick(e.week) * 604800 * ps + tow;
}
void str(ArrowArray *a, const std::string &s) {
    check(ArrowArrayAppendString(a, {s.data(), int64_t(s.size())}));
}
void number(ArrowArray *a, double v) {
    if (std::isfinite(v))
        check(ArrowArrayAppendDouble(a, v));
    else
        check(ArrowArrayAppendNull(a, 1));
}
void integer(ArrowArray *a, int64_t v) { check(ArrowArrayAppendInt(a, v)); }
void decimal(ArrowArray *a, Tick v) {
    ArrowDecimal d;
    ArrowDecimalInit(&d, 128, 38, 12);
    d.words[d.low_word_index] = static_cast<uint64_t>(v);
    d.words[d.high_word_index] = static_cast<uint64_t>(v >> 64);
    check(ArrowArrayAppendDecimal(a, &d));
}
struct Batch {
    ArrowSchema schema{};
    ArrowArray array{};
    ~Batch() {
        if (array.release)
            array.release(&array);
        if (schema.release)
            schema.release(&schema);
    }
    Batch() { ArrowSchemaInit(&schema); }
    Batch(const Batch &) = delete;
    Batch &operator=(const Batch &) = delete;
    void init() {
        ArrowError error{};
        check(ArrowArrayInitFromSchema(&array, &schema, &error));
        check(ArrowArrayStartAppending(&array));
    }
    void finish() {
        ArrowError error{};
        check(ArrowArrayFinishBuildingDefault(&array, &error));
    }
    py::tuple export_array(py::object requested) {
        if (!requested.is_none())
            throw py::value_error("Requested schema conversion is unsupported");
        if (!array.release)
            throw py::value_error("Batch already consumed");
        // Build both owners before transferring the Arrow resources. Allocation
        // or capsule creation failures leave this batch intact.
        auto s = std::make_unique<ArrowSchema>();
        auto a = std::make_unique<ArrowArray>();
        py::capsule sc(s.get(), "arrow_schema", [](PyObject *c) {
            auto p = static_cast<ArrowSchema *>(
                PyCapsule_GetPointer(c, "arrow_schema"));
            if (p) {
                if (p->release)
                    p->release(p);
                delete p;
            }
        });
        auto *schema_owner = s.release();
        py::capsule ac(a.get(), "arrow_array", [](PyObject *c) {
            auto p = static_cast<ArrowArray *>(
                PyCapsule_GetPointer(c, "arrow_array"));
            if (p) {
                if (p->release)
                    p->release(p);
                delete p;
            }
        });
        auto *array_owner = a.release();
        *schema_owner = schema;
        schema.release = nullptr;
        *array_owner = array;
        array.release = nullptr;
        return py::make_tuple(sc, ac);
    }
};
void field(ArrowSchema *s, const char *name, ArrowType type,
           bool nullable = true) {
    check(ArrowSchemaSetType(s, type));
    check(ArrowSchemaSetName(s, name));
    if (!nullable)
        s->flags &= ~ARROW_FLAG_NULLABLE;
}
void decfield(ArrowSchema *s, const char *name, bool nullable = false) {
    check(ArrowSchemaSetTypeDecimal(s, NANOARROW_TYPE_DECIMAL128, 38, 12));
    check(ArrowSchemaSetName(s, name));
    if (!nullable)
        s->flags &= ~ARROW_FLAG_NULLABLE;
}
std::shared_ptr<Batch> observations() {
    auto b = std::make_shared<Batch>();
    check(ArrowSchemaSetTypeStruct(&b->schema, 16));
    auto c = b->schema.children;
    field(c[0], "stream_id", NANOARROW_TYPE_STRING, false);
    decfield(c[1], "gpst");
    field(c[2], "satellite_system", NANOARROW_TYPE_STRING, false);
    field(c[3], "satellite_number", NANOARROW_TYPE_UINT16, false);
    field(c[4], "signal", NANOARROW_TYPE_STRING, false);
    const char *names[] = {"pseudorange_m", "carrier_phase_cycles",
                           "doppler_hz", "cn0_db_hz"};
    const char *quality[] = {"code_quality", "phase_quality",
                             "doppler_quality"};
    for (int i = 0; i < 4; ++i)
        field(c[i + 5], names[i], NANOARROW_TYPE_DOUBLE);
    for (int i = 0; i < 3; ++i) {
        auto q = c[i + 9];
        check(ArrowSchemaSetTypeStruct(q, 4));
        check(ArrowSchemaSetName(q, quality[i]));
        field(q->children[0], "status", NANOARROW_TYPE_STRING, false);
        field(q->children[1], "stddev", NANOARROW_TYPE_FLOAT);
        field(q->children[2], "rinex_ssi", NANOARROW_TYPE_UINT8);
        field(q->children[3], "stddev_is_lower_bound", NANOARROW_TYPE_BOOL);
    }
    auto q = c[12];
    check(ArrowSchemaSetTypeStruct(q, 7));
    check(ArrowSchemaSetName(q, "phase_tracking"));
    field(q->children[0], "loss_of_lock", NANOARROW_TYPE_BOOL);
    field(q->children[1], "half_cycle_ambiguity", NANOARROW_TYPE_BOOL);
    field(q->children[2], "half_cycle_subtracted", NANOARROW_TYPE_BOOL);
    field(q->children[3], "rinex_lli", NANOARROW_TYPE_UINT8);
    auto l = q->children[4];
    check(ArrowSchemaSetTypeStruct(l, 3));
    check(ArrowSchemaSetName(l, "lock"));
    decfield(l->children[0], "lower_s");
    decfield(l->children[1], "upper_s", true);
    field(l->children[2], "representation", NANOARROW_TYPE_STRING, false);
    field(q->children[5], "continuity_counter", NANOARROW_TYPE_UINT32);
    field(q->children[6], "continuity_counter_modulus", NANOARROW_TYPE_UINT32);
    q = c[13];
    check(ArrowSchemaSetTypeStruct(q, 4));
    check(ArrowSchemaSetName(q, "cn0_quality"));
    field(q->children[0], "status", NANOARROW_TYPE_STRING, false);
    field(q->children[1], "stddev", NANOARROW_TYPE_FLOAT);
    field(q->children[2], "rinex_ssi", NANOARROW_TYPE_UINT8);
    field(q->children[3], "stddev_is_lower_bound", NANOARROW_TYPE_BOOL);
    q = c[14];
    check(ArrowSchemaSetTypeStruct(q, 3));
    check(ArrowSchemaSetName(q, "receiver_corrections"));
    field(q->children[0], "code_multipath_m", NANOARROW_TYPE_DOUBLE);
    field(q->children[1], "code_smoothing_m", NANOARROW_TYPE_DOUBLE);
    field(q->children[2], "phase_multipath_cycles", NANOARROW_TYPE_DOUBLE);
    field(c[15], "doppler_variance_factor", NANOARROW_TYPE_FLOAT);
    b->init();
    return b;
}
void append(Batch &b, const cppgnss::Measurement &m, Tick t,
            const std::string &stream) {
    auto c = b.array.children;
    str(c[0], stream);
    decimal(c[1], t);
    str(c[2], m.system);
    integer(c[3], m.satellite);
    str(c[4], m.signal);
    number(c[5], m.code);
    number(c[6], m.phase);
    number(c[7], m.doppler);
    number(c[8], m.cn0);
    int statuses[] = {m.code_status, m.phase_status, 2};
    float sigmas[] = {m.code_sigma, m.phase_sigma, m.doppler_sigma};
    std::optional<bool> bounds[] = {m.code_sigma_lower_bound,
                                    m.phase_sigma_lower_bound,
                                    m.doppler_sigma_lower_bound};
    for (int i = 0; i < 3; ++i) {
        auto q = c[9 + i];
        str(q->children[0], statuses[i] == 0   ? "valid"
                            : statuses[i] == 1 ? "invalid"
                                               : "unknown");
        number(q->children[1], sigmas[i]);
        check(ArrowArrayAppendNull(q->children[2], 1));
        if (std::isfinite(sigmas[i]) && bounds[i].has_value())
            integer(q->children[3], *bounds[i]);
        else
            check(ArrowArrayAppendNull(q->children[3], 1));
        check(ArrowArrayFinishElement(q));
    }
    auto q = c[12];
    check(ArrowArrayAppendNull(q->children[0], 1));
    integer(q->children[1], m.half_ambiguity);
    if (m.half_subtracted)
        integer(q->children[2], *m.half_subtracted);
    else
        check(ArrowArrayAppendNull(q->children[2], 1));
    check(ArrowArrayAppendNull(q->children[3], 1));
    auto l = q->children[4];
    if (m.lock_ms) {
        decimal(l->children[0], Tick(*m.lock_ms) * 1000000000);
        check(ArrowArrayAppendNull(l->children[1], 1));
        str(l->children[2],
            m.lock_lower_bound ? "lower_bound" : "reported_value");
        check(ArrowArrayFinishElement(l));
    } else
        check(ArrowArrayAppendNull(l, 1));
    if (m.continuity_counter) {
        integer(q->children[5], *m.continuity_counter);
        integer(q->children[6], 256);
    } else {
        check(ArrowArrayAppendNull(q->children[5], 1));
        check(ArrowArrayAppendNull(q->children[6], 1));
    }
    check(ArrowArrayFinishElement(q));
    check(ArrowArrayAppendNull(c[13], 1));
    if (m.has_extra) {
        auto x = c[14];
        number(x->children[0], m.code_multipath_m);
        number(x->children[1], m.code_smoothing_m);
        number(x->children[2], m.phase_multipath_cycles);
        check(ArrowArrayFinishElement(x));
    } else
        check(ArrowArrayAppendNull(c[14], 1));
    number(c[15], m.doppler_variance_factor);
    check(ArrowArrayFinishElement(&b.array));
}
std::shared_ptr<Batch> events() {
    auto b = std::make_shared<Batch>();
    check(ArrowSchemaSetTypeStruct(&b->schema, 8));
    auto c = b->schema.children;
    field(c[0], "stream_id", NANOARROW_TYPE_STRING, false);
    field(c[1], "kind", NANOARROW_TYPE_STRING, false);
    field(c[2], "scope", NANOARROW_TYPE_STRING, false);
    decfield(c[3], "gpst");
    field(c[4], "applicability", NANOARROW_TYPE_STRING, false);
    decfield(c[5], "end_gpst", true);
    field(c[6], "evidence", NANOARROW_TYPE_STRING, false);
    check(ArrowSchemaSetTypeStruct(c[7], 1));
    check(ArrowSchemaSetName(c[7], "payload"));
    auto q = c[7]->children[0];
    check(ArrowSchemaSetTypeStruct(q, 2));
    check(ArrowSchemaSetName(q, "epoch_completion"));
    field(q->children[0], "completion", NANOARROW_TYPE_STRING, false);
    field(q->children[1], "completion_basis", NANOARROW_TYPE_STRING, false);
    b->init();
    return b;
}
void complete(Batch &b, Tick t, const std::string &id, bool rawx) {
    auto c = b.array.children;
    str(c[0], id);
    str(c[1], "EPOCH_COMPLETION");
    str(c[2], "OBSERVATION");
    decimal(c[3], t);
    str(c[4], "EPOCH");
    check(ArrowArrayAppendNull(c[5], 1));
    str(c[6], "REPORTED");
    auto q = c[7]->children[0];
    str(q->children[0], "COMPLETE");
    str(q->children[1], rawx ? "RECORD_STRUCTURE" : "PROTOCOL_BOUNDARY");
    check(ArrowArrayFinishElement(q));
    check(ArrowArrayFinishElement(c[7]));
    check(ArrowArrayFinishElement(&b.array));
}
struct Reader {
    cppgnss::StreamDecoder decoder;
    std::string stream;
    unsigned antenna;
    std::optional<cppgnss::Measurements> pending;
    uint64_t pending_start = 0, epochs = 0, unsupported = 0, untimed = 0,
             incomplete = 0, other_antenna = 0, meas3 = 0;
    std::mutex mutex;
    uint64_t excluded = 0;
    std::vector<cppgnss::Measurement> extras;
    bool has_measurements = false;
    uint64_t extra_blocks = 0, extra_matched = 0, extra_unmatched = 0,
             extra_ambiguous = 0, extra_excluded = 0, extra_unsupported = 0;
    void associate_extras() {
        using Key = std::tuple<unsigned, unsigned, unsigned>;
        auto key = [](const auto &m) {
            return Key{m.receiver_channel, m.native_signal, m.antenna};
        };
        std::map<Key, std::vector<cppgnss::Measurement *>> targets;
        std::map<Key, size_t> source_counts;
        for (auto &m : pending->rows)
            targets[key(m)].push_back(&m);
        for (auto &x : extras)
            ++source_counts[key(x)];
        for (auto &x : extras) {
            auto found = targets.find(key(x));
            if (found == targets.end()) {
                ++extra_unmatched;
                continue;
            }
            if (found->second.size() != 1 || source_counts[key(x)] != 1) {
                ++extra_ambiguous;
                continue;
            }
            auto &m = *found->second[0];
            m.has_extra = true;
            m.code_sigma = x.code_sigma;
            m.phase_sigma = x.phase_sigma;
            m.doppler_sigma = x.doppler_sigma;
            m.code_sigma_lower_bound = x.code_sigma_lower_bound;
            m.phase_sigma_lower_bound = x.phase_sigma_lower_bound;
            m.doppler_sigma_lower_bound = x.doppler_sigma_lower_bound;
            m.code_multipath_m = x.code_multipath_m;
            m.code_smoothing_m = x.code_smoothing_m;
            m.phase_multipath_cycles = x.phase_multipath_cycles;
            m.doppler_variance_factor = x.doppler_variance_factor;
            m.continuity_counter = x.continuity_counter;
            if (std::isfinite(m.cn0))
                m.cn0 += x.cn0_increment;
            if (x.lock_ms) {
                m.lock_ms = x.lock_ms;
                m.lock_lower_bound = x.lock_lower_bound;
            }
            ++extra_matched;
        }
        extras.clear();
    }
    Reader(const std::string &protocol, const std::string &id, unsigned ant)
        : decoder(protocol == "ubx" ? cppgnss::Protocol::ubx
                                    : cppgnss::Protocol::sbf),
          stream(id), antenna(ant) {
        if (protocol != "ubx" && protocol != "sbf")
            throw std::invalid_argument("Expected ubx or sbf");
        if (protocol == "ubx" && ant != 0)
            throw std::invalid_argument(
                "RAWX importer supports antenna 0 only");
    }
    std::pair<std::shared_ptr<Batch>, std::shared_ptr<Batch>>
    feed(std::span<const uint8_t> data) {
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock())
            throw std::runtime_error("Concurrent importer use");
        auto out = observations();
        auto ev = events();
        auto emit = [&](const cppgnss::Measurements &e) {
            Tick t;
            try {
                t = time_of(e);
            } catch (const std::runtime_error &) {
                ++untimed;
                return;
            }
            ++epochs;
            complete(*ev, t, stream, !e.tow_ms.has_value());
            for (auto &m : e.rows)
                if (m.antenna == antenna)
                    append(*out, m, t, stream);
                else
                    ++other_antenna;
        };
        decoder.feed(data, [&](const cppgnss::FrameView &f) {
            if (f.protocol == cppgnss::Protocol::sbf && f.id >= 4109 &&
                f.id <= 4113)
                ++meas3;
            auto e = cppgnss::decode_measurements(f);
            auto extra = cppgnss::decode_measurement_extras(f);
            auto adopt = [&](const cppgnss::Measurements &time) {
                if (!pending || pending->week != time.week ||
                    pending->tow_ms != time.tow_ms) {
                    if (has_measurements)
                        ++incomplete;
                    extra_unmatched += extras.size();
                    extras.clear();
                    has_measurements = false;
                    pending = cppgnss::Measurements{};
                    pending->week = time.week;
                    pending->tow_ms = time.tow_ms;
                    pending->tow_seconds = time.tow_seconds;
                    pending_start = f.offset;
                }
            };
            if (extra) {
                ++extra_blocks;
                extra_excluded += extra->excluded;
                extra_unsupported += extra->unsupported;
                adopt(*extra);
                extras.insert(extras.end(), extra->rows.begin(),
                              extra->rows.end());
            }
            if (e) {
                unsupported += e->unsupported;
                excluded += e->excluded;
                if (f.protocol == cppgnss::Protocol::ubx) {
                    emit(*e);
                    return;
                }
                adopt(*e);
                has_measurements = true;
                pending->rows.insert(pending->rows.end(), e->rows.begin(),
                                     e->rows.end());
            }
            if (f.protocol == cppgnss::Protocol::sbf && f.id == 5922 &&
                f.payload.size() >= 6 && pending) {
                uint32_t tow = 0;
                for (int i = 0; i < 4; ++i)
                    tow |= uint32_t(f.payload[i]) << (i * 8);
                uint16_t week = f.payload[4] + 256u * f.payload[5];
                if (week == pending->week && tow == pending->tow_ms) {
                    if (has_measurements) {
                        associate_extras();
                        emit(*pending);
                    } else {
                        extra_unmatched += extras.size();
                        extras.clear();
                    }
                    pending.reset();
                    has_measurements = false;
                }
            }
        });
        out->finish();
        ev->finish();
        return {out, ev};
    }
    py::dict summary() {
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock())
            throw std::runtime_error("Concurrent importer use");
        py::dict d;
        d["source_bytes"] = decoder.bytes;
        d["frames"] = decoder.frames;
        d["invalid_frames"] = decoder.invalid;
        d["noise_bytes"] = decoder.noise;
        d["epochs"] = epochs;
        d["unsupported_signals"] = unsupported;
        d["excluded_signals"] = excluded;
        d["other_antenna"] = other_antenna;
        d["untimed_epochs"] = untimed;
        d["incomplete_epochs"] = incomplete;
        d["pending_epoch"] = bool(pending);
        d["pending_frame_bytes"] = decoder.pending_bytes();
        d["resume_offset"] = pending ? pending_start : decoder.pending_offset();
        d["skipped_protocol_frames"] = decoder.skipped_protocol_frames;
        d["skipped_protocol_bytes"] = decoder.skipped_protocol_bytes;
        d["ignored_meas3_blocks"] = meas3;
        d["measextra_blocks"] = extra_blocks;
        d["measextra_matched"] = extra_matched;
        d["measextra_unmatched"] = extra_unmatched;
        d["measextra_ambiguous"] = extra_ambiguous;
        d["measextra_excluded"] = extra_excluded;
        d["measextra_unsupported"] = extra_unsupported;
        d["measextra_pending"] = extras.size();
        return d;
    }
};
} // namespace
void bind_cnex(py::module_ &m) {
    py::class_<Batch, std::shared_ptr<Batch>>(m, "CnexArrowBatch")
        .def("__arrow_c_array__", &Batch::export_array,
             py::arg("requested_schema") = py::none());
    py::class_<Reader>(m, "CnexObservationReader")
        .def(py::init<const std::string &, const std::string &, unsigned>(),
             py::arg("protocol"), py::arg("stream_id"), py::arg("antenna") = 0)
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
        .def("summary", &Reader::summary);
}
