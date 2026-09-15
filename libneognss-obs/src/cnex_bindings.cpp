// SPDX-License-Identifier: GPL-3.0-only
#include <bit>
#include <boost/int128/int128.hpp>
#include <cppgnss/measurements.hpp>
#include <cppgnss/raw_bits.hpp>
#include <cppgnss/sbf.hpp>
#include <cppgnss/ubx_subframe.hpp>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <nanoarrow/nanoarrow.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <string_view>
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
py::tuple time_parts(Tick time) {
    return py::make_tuple(int64_t(time / ps), int64_t(time % ps));
}
std::optional<int64_t> timegps(const cppgnss::FrameView &f) {
    if (f.protocol != cppgnss::Protocol::ubx || f.id != 0x0120 ||
        f.payload.size() != 16)
        return {};
    auto tow = UBX::read_le<uint32_t>(f.payload, 0);
    auto week = UBX::read_le<int16_t>(f.payload, 8);
    if ((f.payload[11] & 3) != 3 || week < 0 || tow >= 604800000)
        return {};
    return int64_t(week) * 604800000 + tow;
}
// Synchronous navigation blocks only: RawNavBits headers carry SIS time.
std::optional<int64_t> sbf_navigation_time(const cppgnss::FrameView &f) {
    if (f.protocol != cppgnss::Protocol::sbf ||
        (f.id != 4006 && f.id != 4007 && f.id != 5914 && f.id != 5921) ||
        f.payload.size() < 6)
        return {};
    auto tow = UBX::read_le<uint32_t>(f.payload, 0);
    auto week = UBX::read_le<uint16_t>(f.payload, 4);
    if (tow >= 604800000 || week == 65535)
        return {};
    return int64_t(week) * 604800000 + tow;
}
struct Probe {
    cppgnss::StreamDecoder decoder;
    std::mutex mutex;
    std::optional<Tick> observation, navigation;
    explicit Probe(const std::string &protocol)
        : decoder(protocol == "ubx" ? cppgnss::Protocol::ubx
                                    : cppgnss::Protocol::sbf) {
        if (protocol != "ubx" && protocol != "sbf")
            throw std::invalid_argument("Expected ubx or sbf");
    }
    void feed(std::span<const uint8_t> bytes) {
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock())
            throw std::runtime_error("Concurrent probe use");
        decoder.feed(bytes, [&](const cppgnss::FrameView &f) {
            if (observation)
                return;
            try {
                if (auto e = cppgnss::decode_measurements(f))
                    observation = time_of(*e);
                if (!navigation) {
                    if (auto ms = timegps(f))
                        navigation = Tick(*ms) * 1000000000;
                    else if (f.protocol == cppgnss::Protocol::sbf) {
                        if (auto ms = sbf_navigation_time(f))
                            navigation = Tick(*ms) * 1000000000;
                    }
                }
            } catch (const std::runtime_error &) {
                // The probe needs a usable anchor, not a full QA pass.
                // Normal import still diagnoses malformed/unsupported input.
            }
        });
    }
    py::dict result() {
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock())
            throw std::runtime_error("Concurrent probe use");
        py::dict d;
        d["observation"] =
            observation ? py::object(time_parts(*observation)) : py::none();
        d["navigation"] =
            navigation ? py::object(time_parts(*navigation)) : py::none();
        return d;
    }
};
void str(ArrowArray *a, std::string_view s) {
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
    struct Capacity {
        int64_t rows = 0, data_bytes = 0;
        std::vector<Capacity> children;
    };
    static Capacity sizes(const ArrowArray &a, const ArrowSchema &s) {
        Capacity out{a.length};
        if (std::string_view(s.format) == "u" ||
            std::string_view(s.format) == "z")
            out.data_bytes =
                ArrowArrayBuffer(const_cast<ArrowArray *>(&a), 2)->size_bytes;
        for (int64_t i = 0; i < a.n_children; ++i)
            out.children.push_back(sizes(*a.children[i], *s.children[i]));
        return out;
    }
    static void reserve(ArrowArray &a, const Capacity &hint) {
        // Reserve leaf buffers from the preceding bounded batch, including
        // string/binary data. This changes capacity only, not length or
        // validity.
        if (a.n_children == 0 && hint.rows)
            check(ArrowArrayReserve(&a, hint.rows));
        if (hint.data_bytes)
            check(ArrowBufferReserve(ArrowArrayBuffer(&a, 2), hint.data_bytes));
        for (int64_t i = 0;
             i < a.n_children && i < int64_t(hint.children.size()); ++i)
            reserve(*a.children[i], hint.children[i]);
    }
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
    field(c[0], "setup_id", NANOARROW_TYPE_STRING, false);
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
    check(ArrowSchemaSetTypeStruct(q, 4));
    check(ArrowSchemaSetName(q, "receiver_corrections"));
    field(q->children[0], "code_multipath_m", NANOARROW_TYPE_DOUBLE);
    field(q->children[1], "code_smoothing_m", NANOARROW_TYPE_DOUBLE);
    field(q->children[2], "phase_multipath_cycles", NANOARROW_TYPE_DOUBLE);
    field(q->children[3], "code_smoothing_applied", NANOARROW_TYPE_BOOL);
    field(c[15], "doppler_variance_factor", NANOARROW_TYPE_FLOAT);
    b->init();
    return b;
}
void append_details(Batch &b, const cppgnss::Measurement &m) {
    auto c = b.array.children;
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
    if (m.has_extra || m.code_smoothing_applied.has_value()) {
        auto x = c[14];
        number(x->children[0], m.code_multipath_m);
        number(x->children[1], m.code_smoothing_m);
        number(x->children[2], m.phase_multipath_cycles);
        if (m.code_smoothing_applied.has_value())
            integer(x->children[3], *m.code_smoothing_applied);
        else
            check(ArrowArrayAppendNull(x->children[3], 1));
        check(ArrowArrayFinishElement(x));
    } else
        check(ArrowArrayAppendNull(c[14], 1));
    number(c[15], m.doppler_variance_factor);
}
void append_epoch(Batch &b, std::span<const cppgnss::Measurement *const> rows,
                  Tick t, std::string_view setup_id) {
    auto c = b.array.children;
    // Populate the wide scalar columns consecutively; reuse the same epoch
    // context and retain per-observable null/quality handling in the details.
    for (size_t i = 0; i < rows.size(); ++i)
        str(c[0], setup_id);
    for (size_t i = 0; i < rows.size(); ++i)
        decimal(c[1], t);
    for (auto m : rows)
        str(c[2], m->system);
    for (auto m : rows)
        integer(c[3], m->satellite);
    for (auto m : rows)
        str(c[4], m->signal);
    for (auto m : rows)
        number(c[5], m->code);
    for (auto m : rows)
        number(c[6], m->phase);
    for (auto m : rows)
        number(c[7], m->doppler);
    for (auto m : rows)
        number(c[8], m->cn0);
    for (auto m : rows)
        append_details(b, *m);
    // Equivalent to FinishElement for N non-null struct rows, with the same
    // child-length invariant checked once after all columns have been filled.
    auto length = b.array.length + int64_t(rows.size());
    for (int64_t i = 0; i < b.array.n_children; ++i)
        if (c[i]->length != length)
            throw std::runtime_error("Observation column length mismatch");
    auto bitmap = ArrowArrayValidityBitmap(&b.array);
    if (bitmap->buffer.data)
        check(ArrowBitmapAppend(bitmap, 1, rows.size()));
    b.array.length = length;
}
std::shared_ptr<Batch> events() {
    auto b = std::make_shared<Batch>();
    check(ArrowSchemaSetTypeStruct(&b->schema, 8));
    auto c = b->schema.children;
    field(c[0], "setup_id", NANOARROW_TYPE_STRING, false);
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
void complete(Batch &b, Tick t, const std::string &id, bool rawx,
              const char *scope = "OBSERVATION") {
    auto c = b.array.children;
    str(c[0], id);
    str(c[1], "EPOCH_COMPLETION");
    str(c[2], scope);
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
std::shared_ptr<Batch> raw_bits() {
    auto b = std::make_shared<Batch>();
    check(ArrowSchemaSetTypeStruct(&b->schema, 16));
    auto c = b->schema.children;
    field(c[0], "setup_id", NANOARROW_TYPE_STRING, false);
    decfield(c[1], "nav_epoch_gpst");
    field(c[2], "satellite_system", NANOARROW_TYPE_STRING, false);
    field(c[3], "satellite_number", NANOARROW_TYPE_UINT16, false);
    field(c[4], "bitstream_source", NANOARROW_TYPE_LIST, false);
    field(c[4]->children[0], "item", NANOARROW_TYPE_STRING, false);
    for (int i = 5; i <= 8; ++i)
        field(c[i],
              std::array{"signal_composition", "message_family", "body_format",
                         "content_kind"}[i - 5],
              NANOARROW_TYPE_STRING, false);
    field(c[9], "bit_length", NANOARROW_TYPE_UINT32, false);
    field(c[10], "body", NANOARROW_TYPE_BINARY, false);
    field(c[11], "unit_kind", NANOARROW_TYPE_STRING, false);
    field(c[12], "completeness", NANOARROW_TYPE_STRING, false);
    field(c[13], "checks", NANOARROW_TYPE_LIST, false);
    auto q = c[13]->children[0];
    check(ArrowSchemaSetTypeStruct(q, 6));
    check(ArrowSchemaSetName(q, "item"));
    q->flags &= ~ARROW_FLAG_NULLABLE;
    const char *names[] = {"origin", "kind",     "scope",
                           "result", "evidence", "source_field"};
    for (int i = 0; i < 6; ++i)
        field(q->children[i], names[i], NANOARROW_TYPE_STRING, i == 5);
    field(c[14], "receiver_channel", NANOARROW_TYPE_UINT16);
    check(ArrowSchemaSetTypeStruct(c[15], 2));
    check(ArrowSchemaSetName(c[15], "source_diagnostics"));
    field(c[15]->children[0], "sbf_viterbi_count", NANOARROW_TYPE_UINT8);
    field(c[15]->children[1], "sbf_rs_corrected_symbols", NANOARROW_TYPE_UINT8);
    b->init();
    return b;
}
void append_bits(Batch &b, Tick t, const std::string &setup_id,
                 const cppgnss::RawBits &bits) {
    auto c = b.array.children;
    str(c[0], setup_id);
    decimal(c[1], t);
    str(c[2], bits.system);
    integer(c[3], bits.satellite);
    for (const auto &signal : bits.signals)
        str(c[4]->children[0], signal);
    check(ArrowArrayFinishElement(c[4]));
    str(c[5], bits.signals.empty()       ? "unknown"
              : bits.signals.size() == 1 ? "single"
                                         : "combined");
    str(c[6], bits.family);
    str(c[7], bits.format);
    str(c[8], bits.content);
    integer(c[9], bits.bit_length);
    ArrowBufferView view{};
    view.data.as_uint8 = bits.body.data();
    view.size_bytes = bits.body.size();
    check(ArrowArrayAppendBytes(c[10], view));
    str(c[11], bits.unit);
    str(c[12], "complete");
    for (const auto &item : bits.checks) {
        auto q = c[13]->children[0];
        str(q->children[0], item.origin);
        str(q->children[1], item.kind);
        str(q->children[2], item.scope);
        str(q->children[3], item.result);
        str(q->children[4], item.evidence);
        if (!item.source_field.empty())
            str(q->children[5], item.source_field);
        else
            check(ArrowArrayAppendNull(q->children[5], 1));
        check(ArrowArrayFinishElement(q));
    }
    check(ArrowArrayFinishElement(c[13]));
    auto optional_integer = [](ArrowArray *a, auto v) {
        if (v)
            integer(a, *v);
        else
            check(ArrowArrayAppendNull(a, 1));
    };
    optional_integer(c[14], bits.receiver_channel);
    optional_integer(c[15]->children[0], bits.viterbi_count);
    optional_integer(c[15]->children[1], bits.rs_corrected_symbols);
    check(ArrowArrayFinishElement(c[15]));
    check(ArrowArrayFinishElement(&b.array));
}
std::shared_ptr<Batch> measurement_clock() {
    auto b = std::make_shared<Batch>();
    check(ArrowSchemaSetTypeStruct(&b->schema, 4));
    auto c = b->schema.children;
    field(c[0], "setup_id", NANOARROW_TYPE_STRING, false);
    decfield(c[1], "gpst");
    field(c[2], "adjustment_reported", NANOARROW_TYPE_BOOL);
    field(c[3], "cumulative_adjustment_ms_mod256", NANOARROW_TYPE_UINT8);
    b->init();
    return b;
}
void append_clock(Batch &b, const cppgnss::Measurements &e, Tick t,
                  const std::string &id) {
    if (!e.adjustment_reported.has_value() &&
        !e.cumulative_adjustment_ms_mod256.has_value())
        return;
    auto c = b.array.children;
    str(c[0], id);
    decimal(c[1], t);
    if (e.adjustment_reported.has_value())
        integer(c[2], *e.adjustment_reported);
    else
        check(ArrowArrayAppendNull(c[2], 1));
    if (e.cumulative_adjustment_ms_mod256.has_value())
        integer(c[3], *e.cumulative_adjustment_ms_mod256);
    else
        check(ArrowArrayAppendNull(c[3], 1));
    check(ArrowArrayFinishElement(&b.array));
}
struct Reader {
    cppgnss::StreamDecoder decoder;
    std::string setup_id;
    unsigned antenna;
    std::optional<cppgnss::Measurements> pending;
    uint64_t pending_start = 0, epochs = 0, unsupported = 0, untimed = 0,
             incomplete = 0, other_antenna = 0, meas3 = 0;
    std::mutex mutex;
    std::map<std::string, Tick> last_times;
    std::string reversal_axis;
    Tick reversal_previous = 0, reversal_current = 0;
    uint64_t reversal_offset = 0;
    void monotonic(Tick now, const std::string &axis, uint64_t offset) {
        auto previous = last_times.find(axis);
        if (previous != last_times.end() && now < previous->second) {
            reversal_axis = axis;
            reversal_previous = previous->second;
            reversal_current = now;
            reversal_offset = offset;
            throw std::runtime_error("GPST time reversal");
        }
        last_times[axis] = now;
    }
    py::dict time_error() {
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock())
            throw std::runtime_error("Concurrent importer use");
        py::dict d;
        if (!reversal_axis.empty()) {
            d["axis"] = reversal_axis;
            d["previous"] = time_parts(reversal_previous);
            d["current"] = time_parts(reversal_current);
            d["offset"] = reversal_offset;
        }
        return d;
    }
    uint64_t excluded = 0;
    std::vector<cppgnss::Measurement> extras;
    bool has_measurements = false;
    uint64_t extra_blocks = 0, extra_matched = 0, extra_unmatched = 0,
             extra_ambiguous = 0, extra_excluded = 0, extra_unsupported = 0;
    std::optional<int64_t> nav_ms;
    std::deque<cppgnss::RawBits> nav_bits;
    static constexpr size_t backlog_limit = 4 * 1024 * 1024;
    size_t backlog_bytes = 0;
    uint64_t backlog_dropped = 0, backlog_dropped_bytes = 0;
    Tick timeout_ps;
    std::optional<Tick> progress_time;
    std::map<std::string, uint64_t> raw_families;
    std::map<std::string, uint64_t> raw_checks, raw_skipped;
    std::string check_key;
    std::array<Batch::Capacity, 4> capacity_hints;
    std::vector<const cppgnss::Measurement *> selected_rows;
    bool nav_conflict = false;
    std::optional<int64_t> closure_ms;
    uint64_t nav_skip_before = 0, raw_count = 0, raw_untimed = 0,
             raw_unsupported = 0;
    static size_t retained_bytes(const cppgnss::RawBits &b) {
        // Stable byte accounting across checkpoint reconstruction (not
        // allocator capacity).
        size_t n = sizeof(b) + b.body.size() + b.system.size() +
                   b.family.size() + b.format.size() + b.unit.size() +
                   b.content.size();
        n += b.signals.size() * sizeof(std::string);
        for (const auto &s : b.signals)
            n += s.size();
        n += b.checks.size() * sizeof(cppgnss::RawBitsCheck);
        for (const auto &c : b.checks)
            n += c.origin.size() + c.kind.size() + c.scope.size() +
                 c.result.size() + c.evidence.size() + c.source_field.size();
        return n;
    }
    bool usable_anchor() const {
        return nav_ms &&
               (!progress_time ||
                *progress_time - Tick(*nav_ms) * 1000000000 <= timeout_ps);
    }
    void advance(Tick t) {
        if (!progress_time || t > *progress_time)
            progress_time = t;
    }
    void navigation(const cppgnss::FrameView &f, Batch &out, Batch &ev,
                    const std::optional<cppgnss::Measurements> &measurements) {
        // Replayed observation tails must not replay already imported RawBits.
        if (f.offset < nav_skip_before)
            return;
        if (measurements) {
            try {
                advance(time_of(*measurements));
            } catch (const std::runtime_error &) { /* Invalid time is counted by
                                                      observation import. */
            }
        }
        auto emit = [&](int64_t ms, const cppgnss::RawBits &bits) {
            append_bits(out, Tick(ms) * 1000000000, setup_id, bits);
            ++raw_count;
            ++raw_families[bits.family];
            for (const auto &c : bits.checks) {
                check_key.clear();
                for (std::string_view item :
                     {std::string_view(bits.family), std::string_view(c.origin),
                      std::string_view(c.kind), std::string_view(c.scope),
                      std::string_view(c.result)}) {
                    if (!check_key.empty())
                        check_key += '/';
                    check_key += item;
                }
                ++raw_checks[check_key];
            }
        };
        const bool sbf_anchor =
            f.protocol == cppgnss::Protocol::sbf &&
            (f.id == 4006 || f.id == 4007 || f.id == 5914 || f.id == 5921);
        const bool ubx_anchor =
            f.protocol == cppgnss::Protocol::ubx && f.id == 0x0120;
        if (sbf_anchor || ubx_anchor) {
            auto time = sbf_anchor ? sbf_navigation_time(f) : timegps(f);
            nav_ms =
                time; // An explicit invalid anchor disables the previous one.
            if (ubx_anchor) {
                closure_ms = time;
                nav_conflict = !time;
            }
            if (time) {
                monotonic(Tick(*time) * 1000000000, "navigation/receiver",
                          f.offset);
                advance(Tick(*time) * 1000000000);
                if (usable_anchor()) {
                    for (const auto &bits : nav_bits)
                        emit(*time, bits);
                    nav_bits.clear();
                    backlog_bytes = 0;
                }
            }
        }
        auto decoded = cppgnss::decode_raw_bits(f);
        if (decoded.status == cppgnss::RawBitsStatus::unsupported ||
            decoded.status == cppgnss::RawBitsStatus::malformed) {
            ++raw_unsupported;
            ++raw_skipped[std::to_string(f.id) + "/" +
                          std::to_string(f.revision)];
        }
        if (decoded.status == cppgnss::RawBitsStatus::excluded)
            ++raw_skipped["excluded"];
        if (decoded.record) {
            if (usable_anchor())
                emit(*nav_ms, *decoded.record);
            else {
                auto bytes = retained_bytes(*decoded.record);
                while (!nav_bits.empty() &&
                       backlog_bytes + bytes > backlog_limit) {
                    auto removed = retained_bytes(nav_bits.front());
                    backlog_bytes -= removed;
                    backlog_dropped_bytes += removed;
                    ++backlog_dropped;
                    nav_bits.pop_front();
                }
                if (bytes > backlog_limit) {
                    ++backlog_dropped;
                    backlog_dropped_bytes += bytes;
                } else {
                    backlog_bytes += bytes;
                    nav_bits.push_back(std::move(*decoded.record));
                }
            }
        }
        const auto p = f.payload;
        if (f.protocol == cppgnss::Protocol::ubx && f.id == 0x0161 &&
            p.size() == 4) {
            if (closure_ms && !nav_conflict &&
                *closure_ms % 604800000 == UBX::read_le<uint32_t>(p, 0)) {
                complete(ev, Tick(*closure_ms) * 1000000000, setup_id, false,
                         "NAVIGATION");
            }
            closure_ms.reset();
            nav_conflict = false;
        }
    }
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
    Reader(const std::string &protocol, const std::string &id, unsigned ant,
           int64_t period_seconds, int64_t period_ps)
        : decoder(protocol == "ubx" ? cppgnss::Protocol::ubx
                                    : cppgnss::Protocol::sbf),
          setup_id(id), antenna(ant),
          timeout_ps((Tick(period_seconds) * ps + period_ps) * 10) {
        if (period_seconds < 0 || period_ps < 0 || period_ps >= ps ||
            timeout_ps <= 0)
            throw std::invalid_argument("Expected a positive epoch period in "
                                        "exact seconds/picoseconds");
        if (protocol != "ubx" && protocol != "sbf")
            throw std::invalid_argument("Expected ubx or sbf");
        if (protocol == "ubx" && ant != 0)
            throw std::invalid_argument(
                "RAWX importer supports antenna 0 only");
    }
    std::array<std::shared_ptr<Batch>, 4> feed(std::span<const uint8_t> data) {
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock())
            throw std::runtime_error("Concurrent importer use");
        auto out = observations();
        auto ev = events();
        auto raw = raw_bits();
        auto clock = measurement_clock();
        std::array batches{out, ev, raw, clock};
        for (size_t i = 0; i < batches.size(); ++i)
            Batch::reserve(batches[i]->array, capacity_hints[i]);
        auto emit = [&](const cppgnss::Measurements &e) {
            Tick t;
            try {
                t = time_of(e);
            } catch (const std::runtime_error &) {
                ++untimed;
                return;
            }
            ++epochs;
            complete(*ev, t, setup_id, !e.tow_ms.has_value());
            append_clock(*clock, e, t, setup_id);
            selected_rows.clear();
            for (auto &m : e.rows)
                if (m.antenna == antenna)
                    selected_rows.push_back(&m);
                else
                    ++other_antenna;
            append_epoch(*out, selected_rows, t, setup_id);
        };
        decoder.feed(data, [&](const cppgnss::FrameView &f) {
            if (f.protocol == cppgnss::Protocol::sbf && f.id >= 4109 &&
                f.id <= 4113)
                ++meas3;
            auto e = cppgnss::decode_measurements(f);
            navigation(f, *raw, *ev, e);
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
                std::optional<Tick> time;
                try {
                    time = time_of(*e);
                } catch (const std::runtime_error &) {
                    // Existing invalid-time handling below remains
                    // authoritative.
                }
                if (time)
                    monotonic(*time, "observation", f.offset);
                unsupported += e->unsupported;
                excluded += e->excluded;
                if (f.protocol == cppgnss::Protocol::ubx) {
                    emit(*e);
                    return;
                }
                adopt(*e);
                has_measurements = true;
                if (pending->cumulative_adjustment_ms_mod256.has_value() &&
                    pending->cumulative_adjustment_ms_mod256 !=
                        e->cumulative_adjustment_ms_mod256)
                    throw std::runtime_error("Conflicting MeasEpoch clock "
                                             "counters within one epoch");
                pending->cumulative_adjustment_ms_mod256 =
                    e->cumulative_adjustment_ms_mod256;
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
        raw->finish();
        clock->finish();
        for (size_t i = 0; i < batches.size(); ++i)
            capacity_hints[i] =
                Batch::sizes(batches[i]->array, batches[i]->schema);
        return {out, ev, raw, clock};
    }
    py::dict checkpoint() {
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock())
            throw std::runtime_error("Concurrent importer use");
        py::dict state;
        state["nav_ms"] = nav_ms;
        state["nav_conflict"] = nav_conflict;
        state["closure_ms"] = closure_ms;
        state["progress_time"] =
            progress_time ? py::object(time_parts(*progress_time)) : py::none();
        py::dict times;
        for (const auto &[axis, time] : last_times)
            times[axis.c_str()] = time_parts(time);
        state["last_times"] = times;
        py::list bits;
        for (const auto &b : nav_bits) {
            std::vector<std::array<std::string, 6>> checks;
            for (const auto &c : b.checks)
                checks.push_back({c.origin, c.kind, c.scope, c.result,
                                  c.evidence, c.source_field});
            bits.append(py::make_tuple(
                b.system, b.family, b.format, b.unit, b.content, b.satellite,
                b.signals, b.bit_length, b.body, checks, b.receiver_channel,
                b.viterbi_count, b.rs_corrected_symbols));
        }
        state["nav_bits"] = bits;
        auto offset = pending ? pending_start : decoder.pending_offset();
        state["skip_bytes"] = decoder.pending_offset() - offset;
        return state;
    }
    void restore(const py::dict &state) {
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock())
            throw std::runtime_error("Concurrent importer use");
        if (decoder.bytes)
            throw std::runtime_error("Restore before feeding input");
        nav_ms = state["nav_ms"].cast<std::optional<int64_t>>();
        nav_conflict = state["nav_conflict"].cast<bool>();
        closure_ms = state["closure_ms"].cast<std::optional<int64_t>>();
        if (!state["progress_time"].is_none()) {
            auto t = state["progress_time"].cast<std::pair<int64_t, int64_t>>();
            if (t.first < 0 || t.second < 0 || t.second >= ps)
                throw std::runtime_error("Invalid checkpoint progress time");
            progress_time = Tick(t.first) * ps + t.second;
        }
        nav_skip_before = state["skip_bytes"].cast<uint64_t>();
        for (auto item : state["last_times"].cast<py::dict>()) {
            auto value = item.second.cast<py::sequence>();
            auto seconds = value[0].cast<int64_t>(),
                 fraction = value[1].cast<int64_t>();
            if (seconds < 0 || fraction < 0 || fraction >= ps)
                throw std::runtime_error("Invalid checkpoint GPST");
            last_times[item.first.cast<std::string>()] =
                Tick(seconds) * ps + fraction;
        }
        for (auto item : state["nav_bits"].cast<py::list>()) {
            auto b = py::cast<py::sequence>(item);
            cppgnss::RawBits r;
            r.system = b[0].cast<std::string>();
            r.family = b[1].cast<std::string>();
            r.format = b[2].cast<std::string>();
            r.unit = b[3].cast<std::string>();
            r.content = b[4].cast<std::string>();
            r.satellite = b[5].cast<uint16_t>();
            r.signals = b[6].cast<std::vector<std::string>>();
            r.bit_length = b[7].cast<uint32_t>();
            r.body = b[8].cast<std::vector<uint8_t>>();
            r.receiver_channel = b[10].cast<std::optional<uint16_t>>();
            r.viterbi_count = b[11].cast<std::optional<uint8_t>>();
            r.rs_corrected_symbols = b[12].cast<std::optional<uint8_t>>();
            for (auto c : b[9].cast<std::vector<std::array<std::string, 6>>>())
                r.checks.push_back({c[0], c[1], c[2], c[3], c[4], c[5]});
            backlog_bytes += retained_bytes(r);
            if (backlog_bytes > backlog_limit)
                throw std::runtime_error("Invalid navigation checkpoint size");
            nav_bits.push_back(std::move(r));
        }
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
        std::optional<int64_t> safe_day, cursor_day;
        for (const auto &[axis, time] : last_times) {
            auto day = int64_t(time / ps / 86400);
            safe_day = safe_day ? std::min(*safe_day, day) : day;
            cursor_day = cursor_day ? std::max(*cursor_day, day) : day;
        }
        if (pending && safe_day && pending->week < 65535 && pending->tow_ms &&
            *pending->tow_ms < 604800000)
            *safe_day =
                std::min(*safe_day, int64_t(time_of(*pending) / ps / 86400));
        d["safe_day"] = safe_day;
        d["cursor_day"] = cursor_day;
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
        d["raw_bits"] = raw_count;
        d["raw_bits_families"] = raw_families;
        d["raw_bits_checks"] = raw_checks;
        d["raw_bits_skipped"] = raw_skipped;
        d["raw_bits_untimed"] = raw_untimed;
        d["raw_bits_unsupported"] = raw_unsupported;
        d["raw_bits_pending"] = nav_bits.size();
        d["raw_bits_pending_bytes"] = backlog_bytes;
        d["raw_bits_dropped"] = backlog_dropped;
        d["raw_bits_dropped_bytes"] = backlog_dropped_bytes;
        return d;
    }
};
} // namespace
void bind_cnex(py::module_ &m) {
    py::class_<Probe>(m, "CnexTimeProbe")
        .def(py::init<const std::string &>(), py::arg("protocol"))
        .def("feed",
             [](Probe &r, py::bytes bytes) {
                 char *p;
                 Py_ssize_t n;
                 if (PyBytes_AsStringAndSize(bytes.ptr(), &p, &n))
                     throw py::error_already_set();
                 py::gil_scoped_release release;
                 r.feed({reinterpret_cast<const uint8_t *>(p), size_t(n)});
             })
        .def("result", &Probe::result);
    py::class_<Batch, std::shared_ptr<Batch>>(m, "CnexArrowBatch")
        .def("__arrow_c_array__", &Batch::export_array,
             py::arg("requested_schema") = py::none());
    py::class_<Reader>(m, "CnexObservationReader")
        .def(py::init<const std::string &, const std::string &, unsigned,
                      int64_t, int64_t>(),
             py::arg("protocol"), py::arg("setup_id"), py::arg("antenna"),
             py::arg("period_seconds"), py::arg("period_ps"))
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
        .def("summary", &Reader::summary)
        .def("time_error", &Reader::time_error)
        .def("checkpoint", &Reader::checkpoint)
        .def("restore", &Reader::restore);
}
