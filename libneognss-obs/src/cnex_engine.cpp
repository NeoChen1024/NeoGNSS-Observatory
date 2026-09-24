// SPDX-License-Identifier: GPL-3.0-only
#include "cnex_build.hpp"
#include "cnex_decode.hpp"
#include "receiver_time.hpp"
#include <bit>
#include <boost/int128/int128.hpp>
#include <cppgnss/sbf.hpp>
#include <cppgnss/sbf_pvt_gen.hpp>
#include <cppgnss/sbf_receiver_time_gen.hpp>
#include <cppgnss/sbf_status_gen.hpp>
#include <cppgnss/ubx_subframe.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <nanoarrow/nanoarrow.h>
#include <neognss_obs/cnex_engine.hpp>
#include <neognss_obs/measurements.hpp>
#include <neognss_obs/raw_bits.hpp>
#include <string_view>
#include <tuple>
#include <vector>

namespace neognss_obs::cnex_detail {
using Json = nlohmann::json;
using Tick = boost::int128::int128;
constexpr int64_t ps = 1000000000000LL;
void check(int result) {
    if (result)
        throw std::runtime_error("Arrow construction failed: " +
                                 std::to_string(result));
}
Tick time_of(const neognss_obs::Measurements &e) {
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
Json time_parts(Tick time) {
    return Json::array({int64_t(time / ps), int64_t(time % ps)});
}
std::optional<int64_t> timegps(const cppgnss::FrameView &f) {
    if (f.protocol() != cppgnss::Protocol::ubx || f.id() != 0x0120 ||
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
    if (f.protocol() != cppgnss::Protocol::sbf ||
        (f.id() != 4006 && f.id() != 4007 && f.id() != 5914 &&
         f.id() != 5921) ||
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
                if (auto e = neognss_obs::decode_measurements(f))
                    observation = time_of(*e);
                if (!navigation) {
                    if (auto ms = timegps(f))
                        navigation =
                            Tick(*ms) * 1000000000 +
                            Tick(UBX::read_le<int32_t>(f.payload, 4)) * 1000;
                    else if (f.protocol() == cppgnss::Protocol::sbf) {
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
    Json result() {
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock())
            throw std::runtime_error("Concurrent probe use");
        Json d = Json::object();
        d["observation"] =
            observation ? time_parts(*observation) : Json(nullptr);
        d["navigation"] = navigation ? time_parts(*navigation) : Json(nullptr);
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
struct Batch : CnexBatch {
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
    check(ArrowSchemaSetTypeStruct(&b->schema, 12));
    auto c = b->schema.children;
    field(c[0], "setup_id", NANOARROW_TYPE_STRING, false);
    decfield(c[1], "gpst");
    field(c[2], "satellite_system", NANOARROW_TYPE_STRING, false);
    field(c[3], "satellite_number", NANOARROW_TYPE_UINT16, false);
    field(c[4], "signal", NANOARROW_TYPE_STRING, false);
    const char *names[] = {"pseudorange_m", "carrier_phase_cycles",
                           "doppler_hz", "cn0_db_hz"};
    for (int i = 0; i < 4; ++i)
        field(c[i + 5], names[i], NANOARROW_TYPE_DOUBLE);
    auto q = c[9];
    check(ArrowSchemaSetTypeStruct(q, 8));
    check(ArrowSchemaSetName(q, "quality"));
    field(q->children[0], "code_valid", NANOARROW_TYPE_BOOL);
    field(q->children[1], "phase_valid", NANOARROW_TYPE_BOOL);
    const char *sigma[] = {"code_stddev_m", "phase_stddev_cycles",
                           "doppler_stddev_hz"};
    const char *bound[] = {"code_stddev_is_lower_bound",
                           "phase_stddev_is_lower_bound",
                           "doppler_stddev_is_lower_bound"};
    for (int i = 0; i < 3; ++i) {
        field(q->children[2 + 2 * i], sigma[i], NANOARROW_TYPE_FLOAT);
        field(q->children[3 + 2 * i], bound[i], NANOARROW_TYPE_BOOL);
    }
    q = c[10];
    check(ArrowSchemaSetTypeStruct(q, 6));
    check(ArrowSchemaSetName(q, "tracking"));
    field(q->children[0], "half_cycle_ambiguity", NANOARROW_TYPE_BOOL);
    field(q->children[1], "half_cycle_subtracted", NANOARROW_TYPE_BOOL);
    decfield(q->children[2], "lock_duration_s", true);
    field(q->children[3], "lock_duration_is_lower_bound", NANOARROW_TYPE_BOOL);
    field(q->children[4], "continuity_counter", NANOARROW_TYPE_UINT32);
    field(q->children[5], "continuity_counter_modulus", NANOARROW_TYPE_UINT32);
    q = c[11];
    check(ArrowSchemaSetTypeStruct(q, 4));
    check(ArrowSchemaSetName(q, "receiver_corrections"));
    field(q->children[0], "code_multipath_m", NANOARROW_TYPE_DOUBLE);
    field(q->children[1], "code_smoothing_m", NANOARROW_TYPE_DOUBLE);
    field(q->children[2], "phase_multipath_cycles", NANOARROW_TYPE_DOUBLE);
    field(q->children[3], "code_smoothing_applied", NANOARROW_TYPE_BOOL);
    b->init();
    return b;
}
void finish_struct(ArrowArray &array, int64_t count) {
    const auto length = array.length + count;
    for (int64_t i = 0; i < array.n_children; ++i)
        if (array.children[i]->length != length)
            throw std::runtime_error("Struct column length mismatch");
    auto bitmap = ArrowArrayValidityBitmap(&array);
    if (bitmap->buffer.data)
        check(ArrowBitmapAppend(bitmap, 1, count));
    array.length = length;
}
void append_details(Batch &b,
                    std::span<const neognss_obs::Measurement *const> rows) {
    auto c = b.array.children;
    const auto count = int64_t(rows.size());
    auto flag = [](ArrowArray *array, std::optional<bool> value) {
        if (value.has_value())
            integer(array, *value);
        else
            check(ArrowArrayAppendNull(array, 1));
    };
    auto q = c[9];
    for (auto m : rows) {
        flag(q->children[0], m->code_valid);
        flag(q->children[1], m->phase_valid);
        const float sigma[] = {m->code_sigma, m->phase_sigma, m->doppler_sigma};
        const std::optional<bool> bound[] = {m->code_sigma_lower_bound,
                                             m->phase_sigma_lower_bound,
                                             m->doppler_sigma_lower_bound};
        for (int i = 0; i < 3; ++i) {
            number(q->children[2 + 2 * i], sigma[i]);
            flag(q->children[3 + 2 * i],
                 std::isfinite(sigma[i]) ? bound[i] : std::nullopt);
        }
    }
    finish_struct(*q, count);
    q = c[10];
    for (auto m : rows) {
        integer(q->children[0], m->half_ambiguity);
        flag(q->children[1], m->half_subtracted);
        if (m->lock_ms)
            decimal(q->children[2], Tick(*m->lock_ms) * 1000000000);
        else
            check(ArrowArrayAppendNull(q->children[2], 1));
        flag(q->children[3], m->lock_ms
                                 ? std::optional<bool>(m->lock_lower_bound)
                                 : std::nullopt);
        if (m->continuity_counter) {
            integer(q->children[4], *m->continuity_counter);
            integer(q->children[5], 256);
        } else {
            check(ArrowArrayAppendNull(q->children[4], 1));
            check(ArrowArrayAppendNull(q->children[5], 1));
        }
    }
    finish_struct(*q, count);
    for (auto m : rows) {
        auto x = c[11];
        if (m->has_extra || m->code_smoothing_applied.has_value()) {
            number(x->children[0], m->code_multipath_m);
            number(x->children[1], m->code_smoothing_m);
            number(x->children[2], m->phase_multipath_cycles);
            flag(x->children[3], m->code_smoothing_applied);
            check(ArrowArrayFinishElement(x));
        } else {
            // Reserve the bool child before nanoarrow's bulk-null fill.
            auto buffer = ArrowArrayBuffer(x->children[3], 1);
            auto bytes = (x->children[3]->length + 1 + 7) / 8;
            if (bytes > buffer->size_bytes)
                check(ArrowBufferAppendFill(buffer, 0,
                                            bytes - buffer->size_bytes));
            check(ArrowArrayAppendNull(x, 1));
        }
    }
}
void append_epoch(Batch &b,
                  std::span<const neognss_obs::Measurement *const> rows, Tick t,
                  std::string_view setup_id) {
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
    append_details(b, rows);
    // Equivalent to FinishElement for N non-null struct rows, with the same
    // child-length invariant checked once after all columns have been filled.
    finish_struct(b.array, rows.size());
}
std::shared_ptr<Batch> events() {
    auto b = std::make_shared<Batch>();
    check(ArrowSchemaSetTypeStruct(&b->schema, 10));
    auto c = b->schema.children;
    field(c[0], "setup_id", NANOARROW_TYPE_STRING, false);
    field(c[1], "kind", NANOARROW_TYPE_STRING, false);
    field(c[2], "scope", NANOARROW_TYPE_STRING, false);
    decfield(c[3], "gpst", true);
    field(c[4], "applicability", NANOARROW_TYPE_STRING, false);
    decfield(c[5], "end_gpst", true);
    field(c[6], "evidence", NANOARROW_TYPE_STRING, false);
    check(ArrowSchemaSetTypeStruct(c[7], 2));
    check(ArrowSchemaSetName(c[7], "payload"));
    auto q = c[7]->children[0];
    check(ArrowSchemaSetTypeStruct(q, 2));
    check(ArrowSchemaSetName(q, "epoch_completion"));
    field(q->children[0], "completion", NANOARROW_TYPE_STRING, false);
    field(q->children[1], "completion_basis", NANOARROW_TYPE_STRING, false);
    field(c[7]->children[1], "restart_reason", NANOARROW_TYPE_STRING);
    decfield(c[8], "receiver_uptime_s", true);
    field(c[9], "_archive_day", NANOARROW_TYPE_INT64, false);
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
    check(ArrowArrayAppendNull(c[7]->children[1], 1));
    check(ArrowArrayFinishElement(c[7]));
    check(ArrowArrayAppendNull(c[8], 1));
    integer(c[9], int64_t(t / ps / 86400));
    check(ArrowArrayFinishElement(&b.array));
}
std::shared_ptr<Batch> raw_bits() {
    auto b = std::make_shared<Batch>();
    check(ArrowSchemaSetTypeStruct(&b->schema, 19));
    auto c = b->schema.children;
    field(c[0], "setup_id", NANOARROW_TYPE_STRING, false);
    decfield(c[1], "nav_epoch_gpst", true);
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
    decfield(c[16], "receiver_uptime_s", true);
    field(c[17], "uptime_basis", NANOARROW_TYPE_STRING);
    field(c[18], "_archive_day", NANOARROW_TYPE_INT64, false);
    b->init();
    return b;
}
void append_bits(Batch &b, std::optional<Tick> t, const std::string &setup_id,
                 const neognss_obs::RawBits &bits, std::optional<Tick> uptime,
                 int64_t archive_day) {
    auto c = b.array.children;
    str(c[0], setup_id);
    if (t)
        decimal(c[1], *t);
    else
        check(ArrowArrayAppendNull(c[1], 1));
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
    if (uptime) {
        decimal(c[16], *uptime);
        str(c[17], "ASSOCIATED");
    } else {
        check(ArrowArrayAppendNull(c[16], 1));
        check(ArrowArrayAppendNull(c[17], 1));
    }
    integer(c[18], t ? int64_t(*t / ps / 86400) : archive_day);
    check(ArrowArrayFinishElement(&b.array));
}
std::optional<Tick> scaled(double value, int64_t scale) {
    if (!std::isfinite(value) || value == -2e10)
        return {};
    auto bits = std::bit_cast<uint64_t>(value);
    bool negative = bits >> 63;
    int exponent = int((bits >> 52) & 2047);
    uint64_t mantissa = bits & ((uint64_t(1) << 52) - 1);
    if (exponent)
        mantissa |= uint64_t(1) << 52;
    int shift = (exponent ? exponent - 1023 : -1022) - 52;
    Tick n = Tick(mantissa) * scale, result = 0;
    if (shift >= 0) {
        if (shift >= 63 || n > (Tick(1) << (126 - shift)))
            throw std::runtime_error("Telemetry numeric range exceeded");
        result = n << shift;
    } else if (-shift < 127) {
        int k = -shift;
        result = n >> k;
        Tick remainder = n - (result << k), half = Tick(1) << (k - 1);
        if (remainder > half || (remainder == half && (result & 1) != 0))
            ++result;
    }
    return negative ? -result : result;
}
void optional_decimal(ArrowArray *a, std::optional<Tick> t) {
    if (t)
        decimal(a, *t);
    else
        check(ArrowArrayAppendNull(a, 1));
}
void str(Json *value, std::string_view text) { *value = std::string(text); }
void integer(Json *value, int64_t number) { *value = number; }
void number(Json *value, double number) {
    *value = std::isfinite(number) ? Json(number) : Json(nullptr);
}
void decimal(Json *value, Tick time) { *value = time_parts(time); }
void optional_decimal(Json *value, std::optional<Tick> time) {
    *value = time ? time_parts(*time) : Json(nullptr);
}
#include "cnex_telemetry.hpp"

struct ObservationOutput {
    Tick time;
    std::vector<neognss_obs::Measurement> rows;
};
struct RawOutput {
    std::optional<Tick> time, uptime;
    int64_t archive_day;
    neognss_obs::RawBits bits;
};
struct Reader {
    cppgnss::StreamDecoder decoder;
    neognss_obs::CnexDecodePool decode_pool;
    neognss_obs::CnexBuildLane build_lane;
    unsigned decode_workers;
    std::string setup_id;
    unsigned antenna;
    std::optional<neognss_obs::Measurements> pending;
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
    Json time_error() {
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock())
            throw std::runtime_error("Concurrent importer use");
        Json d = Json::object();
        if (!reversal_axis.empty()) {
            d["axis"] = reversal_axis;
            d["previous"] = time_parts(reversal_previous);
            d["current"] = time_parts(reversal_current);
            d["offset"] = reversal_offset;
        }
        return d;
    }
    uint64_t excluded = 0;
    std::vector<neognss_obs::Measurement> extras;
    bool has_measurements = false;
    uint64_t extra_blocks = 0, extra_matched = 0, extra_unmatched = 0,
             extra_ambiguous = 0, extra_excluded = 0, extra_unsupported = 0;
    std::optional<int64_t> nav_ms;
    neognss_obs::ReceiverTime timeline;
    uint64_t restarts = 0;
    bool new_navigation = false;
    std::map<std::string, uint64_t> raw_families;
    std::map<std::string, uint64_t> raw_checks, raw_skipped;
    std::string check_key;
    std::array<Batch::Capacity, 4> capacity_hints;
    TelemetryAssembler telemetry;
    using JoinEntry = std::pair<uint32_t, neognss_obs::Measurement *>;
    std::vector<JoinEntry> join_targets, join_sources;
    bool nav_conflict = false;
    std::optional<int64_t> closure_ms;
    std::optional<Tick> closure_time;
    uint64_t nav_skip_before = 0, raw_count = 0, raw_untimed = 0,
             raw_unsupported = 0;
    void estimates_frame(const cppgnss::FrameView &f) {
        if (f.offset < nav_skip_before)
            return;
        const bool ubx = f.protocol() == cppgnss::Protocol::ubx;
        const bool pulse = ubx ? f.id() == 0x0d01 : f.id() == 5911;
        const bool clock =
            ubx ? f.id() == 0x0122 : (f.id() == 4006 || f.id() == 4007);
        if (!pulse && !clock)
            return;
        const auto p = f.payload;
        if (ubx && p.size() != (pulse ? 16 : 20))
            return;
        double sbf_offset = 0, sbf_bias = 0, sbf_drift = 0;
        unsigned sbf_system = 0, sbf_sync_age = 0, sbf_error = 0;
        if (!ubx) {
            if (pulse) {
                auto parsed = cppgnss::parse<cppgnss::SBF::xPPSOffset>(f);
                if (!parsed)
                    return;
                sbf_offset = parsed.value().Offset;
                sbf_sync_age = parsed.value().SyncAge;
                sbf_system = parsed.value().TimeScale;
            } else {
                auto accept = [&](const auto &parsed) {
                    if (!parsed)
                        return false;
                    sbf_bias = parsed.value().RxClkBias;
                    sbf_drift = parsed.value().RxClkDrift;
                    sbf_system = parsed.value().TimeSystem;
                    sbf_error = parsed.value().Error;
                    return true;
                };
                if (f.id() == 4006) {
                    if (!accept(cppgnss::parse<cppgnss::SBF::PVTCartesian>(f)))
                        return;
                } else if (!accept(
                               cppgnss::parse<cppgnss::SBF::PVTGeodetic>(f)))
                    return;
            }
        }
        std::optional<Tick> t;
        std::string reference = "UNKNOWN";
        if (!ubx) {
            auto tow = UBX::read_le<uint32_t>(p, 0);
            auto week = UBX::read_le<uint16_t>(p, 4);
            if (tow < 604800000 && week != 65535)
                t = (Tick(week) * 604800000 + tow) * 1000000000;
            int system = int(sbf_system);
            if (pulse)
                reference = system == 1     ? "GPST"
                            : system == 2   ? "UTC"
                            : system == 3   ? "RECEIVER"
                            : system == 5   ? "GST"
                            : system == 6   ? "BDT"
                            : system == 100 ? "FUGRO_ATOMICHRON"
                                            : "UNKNOWN";
            else
                reference = system == 0   ? "GPST"
                            : system == 1 ? "GST"
                            : system == 4 ? "BDT"
                                          : "UNKNOWN";
        } else if (pulse) {
            const unsigned flags = p[14], ref = p[15];
            reference = flags & 1         ? "UTC"
                        : (ref & 15) == 0 ? "GPST"
                        : (ref & 15) == 2 ? "BDT"
                        : (ref & 15) == 3 ? "GST"
                                          : "UNKNOWN";
            auto tow = UBX::read_le<uint32_t>(p, 0);
            // Other time bases require an explicit conversion; never relabel
            // them as GPST or replace a target pulse with message-arrival time.
            if (!(flags & 32) && reference == "GPST" && tow < 604800000) {
                auto sub = UBX::read_le<uint32_t>(p, 4);
                Tick numerator = Tick(sub) * 1000000000;
                Tick fraction = numerator >> 32,
                     remainder = numerator - (fraction << 32);
                if (remainder > (Tick(1) << 31) ||
                    (remainder == (Tick(1) << 31) && (fraction & 1) != 0))
                    ++fraction;
                t = (Tick(UBX::read_le<uint16_t>(p, 12)) * 604800000 + tow) *
                        1000000000 +
                    fraction;
            }
        } else {
            reference = "GPST";
            auto anchor = timeline.anchor();
            auto tow = UBX::read_le<uint32_t>(p, 0);
            if (anchor && tow < 604800000) {
                Tick week = *anchor / ps / 604800;
                Tick candidate = (week * 604800000 + tow) * 1000000000;
                Tick delta = candidate - *anchor;
                if (delta > Tick(302400) * ps)
                    candidate -= Tick(604800) * ps;
                if (delta < -Tick(302400) * ps)
                    candidate += Tick(604800) * ps;
                if (candidate - *anchor <= timeline.timeout &&
                    *anchor - candidate <= timeline.timeout)
                    t = candidate;
            }
        }
        Json report = Json::object();
        std::array<Json *, 16> c{&report["setup_id"],
                                 &report["gpst"],
                                 &report["receiver_uptime_s"],
                                 &report["time_basis"],
                                 &report["uptime_basis"],
                                 &report["source_message"],
                                 &report["reference_time_scale"],
                                 &report["value7"],
                                 &report["value8"],
                                 &report["value9"],
                                 &report["value10"],
                                 &report["value11"],
                                 &report["value12"],
                                 &report["value13"],
                                 &report["value14"],
                                 &report["value15"]};
        str(c[0], setup_id);
        optional_decimal(c[1], t);
        auto uptime = timeline.associated_uptime();
        optional_decimal(c[2], uptime);
        str(c[3], t ? (ubx && clock ? "NAVIGATION" : "SOURCE") : "UNKNOWN");
        if (uptime)
            str(c[4], "ASSOCIATED");
        else
            *c[4] = nullptr;
        str(c[5], ubx              ? (pulse ? "UBX-TIM-TP" : "UBX-NAV-CLOCK")
                  : pulse          ? "SBF-xPPSOffset"
                  : f.id() == 4006 ? "SBF-PVTCartesian"
                                   : "SBF-PVTGeodetic");
        str(c[6], reference);
        if (pulse) {
            bool valid =
                ubx ? !(p[14] & 16) : scaled(sbf_offset, 1000).has_value();
            optional_decimal(
                c[7], valid ? (ubx ? std::optional<Tick>(
                                         -Tick(UBX::read_le<int32_t>(p, 8)))
                                   : scaled(sbf_offset, 1000))
                            : std::nullopt);
            integer(c[8], valid);
            if (ubx) {
                integer(c[9], !(p[14] & 32));
                str(c[10], std::array{"UNAVAILABLE", "NOT_ACTIVE", "ACTIVE",
                                      "UNKNOWN"}[(p[14] >> 2) & 3]);
                if (p[14] & 1)
                    integer(c[11], p[15] >> 4);
                else
                    *c[11] = nullptr;
                *c[12] = nullptr;
                *c[13] = nullptr;
                integer(c[15], bool(p[14] & 2));
            } else {
                for (int i = 9; i <= 11; ++i)
                    *c[i] = nullptr;
                decimal(c[12], Tick(sbf_sync_age) * ps);
                integer(c[13], sbf_sync_age == 255);
                *c[15] = nullptr;
            }
            integer(c[14], t ? int64_t(*t / ps / 86400) : timeline.archive_day);
        } else if (ubx) {
            decimal(c[7], Tick(UBX::read_le<int32_t>(p, 4)) * 1000);
            integer(c[8], int64_t(UBX::read_le<int32_t>(p, 8)) * 1000000);
            decimal(c[9], Tick(UBX::read_le<uint32_t>(p, 12)) * 1000);
            integer(c[10], int64_t(UBX::read_le<uint32_t>(p, 16)) * 1000);
            integer(c[11], t ? int64_t(*t / ps / 86400) : timeline.archive_day);
        } else {
            bool valid = sbf_error == 0;
            optional_decimal(c[7], valid ? scaled(sbf_bias, 1000000000)
                                         : std::nullopt);
            auto drift = valid ? scaled(sbf_drift, 1000000000) : std::nullopt;
            if (drift) {
                if (*drift < std::numeric_limits<int64_t>::min() ||
                    *drift > std::numeric_limits<int64_t>::max())
                    throw std::runtime_error(
                        "Clock frequency outside int64 range");
                integer(c[8], int64_t(*drift));
            } else
                *c[8] = nullptr;
            *c[9] = nullptr;
            *c[10] = nullptr;
            integer(c[11], t ? int64_t(*t / ps / 86400) : timeline.archive_day);
        }

        telemetry.estimate(std::move(report), pulse);
    }
    void restart(neognss_obs::ReceiverTime::Restart reason,
                 std::optional<Tick> sample, Tick uptime, Batch &ev) {
        if (reason != neognss_obs::ReceiverTime::Restart::none) {
            ++restarts;
            nav_ms.reset();
            closure_ms.reset();
            closure_time.reset();
            auto c = ev.array.children;
            str(c[0], setup_id);
            str(c[1], "RECEIVER_RESTART");
            str(c[2], "RECEIVER");
            if (sample)
                decimal(c[3], *sample);
            else
                check(ArrowArrayAppendNull(c[3], 1));
            str(c[4], "POINT");
            check(ArrowArrayAppendNull(c[5], 1));
            str(c[6], "INFERRED");
            check(ArrowArrayAppendNull(c[7]->children[0], 1));
            str(c[7]->children[1],
                reason == neognss_obs::ReceiverTime::Restart::uptime_decrease
                    ? "UPTIME_DECREASE"
                    : "GPST_UPTIME_OFFSET_JUMP");
            check(ArrowArrayFinishElement(c[7]));
            decimal(c[8], uptime);
            integer(c[9], timeline.archive_day);
            check(ArrowArrayFinishElement(&ev.array));
        }
    }
    void status(const cppgnss::FrameView &f, Batch &ev) {
        if (f.offset < nav_skip_before)
            return;
        const bool ubx = f.protocol() == cppgnss::Protocol::ubx;
        if ((ubx && f.id() != 0x0a39) || (!ubx && f.id() != 4014))
            return;
        Tick uptime;
        double temperature;
        std::optional<bool> fine;
        std::optional<Tick> sample;
        if (ubx) {
            if (f.payload.size() != 24 || f.payload[0] != 1)
                return;
            uptime = Tick(UBX::read_le<uint32_t>(f.payload, 8)) * ps;
            temperature = UBX::read_le<int8_t>(f.payload, 18);
            sample = {};
        } else {
            auto block = cppgnss::parse<cppgnss::SBF::ReceiverStatus>(f);
            if (!block)
                return;
            uptime = Tick(block.value().UpTime) * ps;
            auto temp = double(block.value().Temperature);
            temperature =
                temp ? temp - 100 : std::numeric_limits<double>::quiet_NaN();
            fine = bool(block.value().RxState.FineTime);
            auto tow = UBX::read_le<uint32_t>(f.payload, 0);
            auto week = UBX::read_le<uint16_t>(f.payload, 4);
            if (*fine && tow < 604800000 && week != 65535) {
                sample = (Tick(week) * 604800000 + tow) * 1000000000;
                timeline.advance(*sample);
            }
        }
        auto paired = (!ubx || new_navigation) ? sample : std::nullopt;
        auto reason = timeline.report_uptime(uptime, paired, !ubx);
        new_navigation = false;

        restart(reason, sample, uptime, ev);
        Json report = {{"gpst", sample ? time_parts(*sample) : Json(nullptr)},
                       {"receiver_uptime_s", time_parts(uptime)}};
        number(&report["receiver_temperature_c"], temperature);
        if (ubx) {
            report["cpu_load_percent"] = f.payload[2];
        } else {
            const auto block = cppgnss::parse<cppgnss::SBF::ReceiverStatus>(f);
            const auto load = block.value().CPULoad;
            report["cpu_load_percent"] =
                load == 255 ? Json(nullptr) : Json(load);
        }
        telemetry.status(std::move(report), ubx);
    }

    void
    navigation(const cppgnss::FrameView &f, std::vector<RawOutput> &out,
               Batch &ev,
               const std::optional<neognss_obs::Measurements> &measurements,
               neognss_obs::RawBitsResult &decoded) {
        // Replayed observation tails must not replay already imported RawBits.
        if (f.offset < nav_skip_before)
            return;
        if (measurements) {
            try {
                timeline.advance(time_of(*measurements));
            } catch (const std::runtime_error &) { /* Invalid time is counted by
                                                      observation import. */
            }
        }
        auto emit = [&](std::optional<Tick> t, neognss_obs::RawBits &bits) {
            if (!t)
                ++raw_untimed;
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
            out.push_back({t, timeline.associated_uptime(),
                           timeline.archive_day, std::move(bits)});
        };
        const bool sbf_anchor = f.protocol() == cppgnss::Protocol::sbf &&
                                (f.id() == 4006 || f.id() == 4007 ||
                                 f.id() == 5914 || f.id() == 5921);
        const bool ubx_anchor =
            f.protocol() == cppgnss::Protocol::ubx && f.id() == 0x0120;
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
            }
            auto precise = time ? std::optional<Tick>(Tick(*time) * 1000000000)
                                : std::nullopt;
            if (precise && ubx_anchor)
                *precise += Tick(UBX::read_le<int32_t>(f.payload, 4)) * 1000;
            if (ubx_anchor)
                closure_time = precise;
            timeline.set_navigation(precise);
            new_navigation = bool(time);
        }
        if (decoded.status == neognss_obs::RawBitsStatus::unsupported ||
            decoded.status == neognss_obs::RawBitsStatus::malformed) {
            ++raw_unsupported;
            ++raw_skipped[std::to_string(f.id()) + "/" +
                          std::to_string(f.revision())];
        }
        if (decoded.status == neognss_obs::RawBitsStatus::excluded)
            ++raw_skipped["excluded"];
        if (decoded.record)
            emit(timeline.anchor(), *decoded.record);
        const auto p = f.payload;
        if (f.protocol() == cppgnss::Protocol::ubx && f.id() == 0x0161 &&
            p.size() == 4) {
            if (closure_ms && closure_time && !nav_conflict &&
                *closure_ms % 604800000 == UBX::read_le<uint32_t>(p, 0)) {
                complete(ev, *closure_time, setup_id, false, "NAVIGATION");
            }
            closure_ms.reset();
            closure_time.reset();
            nav_conflict = false;
        }
    }
    void associate_extras() {
        auto key = [](const auto &m) {
            return (uint32_t(m.receiver_channel) << 16) |
                   (uint32_t(m.native_signal) << 8) | m.antenna;
        };
        join_targets.clear();
        join_sources.clear();
        for (auto &m : pending->rows)
            join_targets.emplace_back(key(m), &m);
        for (auto &x : extras)
            join_sources.emplace_back(key(x), &x);
        auto less = [](const JoinEntry &a, const JoinEntry &b) {
            return a.first < b.first;
        };
        std::sort(join_targets.begin(), join_targets.end(), less);
        std::sort(join_sources.begin(), join_sources.end(), less);
        size_t target = 0;
        for (size_t source = 0; source < join_sources.size();) {
            const auto k = join_sources[source].first;
            size_t end = source + 1;
            while (end < join_sources.size() && join_sources[end].first == k)
                ++end;
            const auto count = end - source;
            auto &x = *join_sources[source].second;
            source = end;
            while (target < join_targets.size() &&
                   join_targets[target].first < k)
                ++target;
            if (target == join_targets.size() ||
                join_targets[target].first != k) {
                extra_unmatched += count;
                continue;
            }
            if (count != 1 || (target + 1 < join_targets.size() &&
                               join_targets[target + 1].first == k)) {
                extra_ambiguous += count;
                continue;
            }
            auto &m = *join_targets[target].second;
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
           int64_t period_seconds, int64_t period_ps, unsigned workers)
        : decoder(protocol == "ubx" ? cppgnss::Protocol::ubx
                                    : cppgnss::Protocol::sbf),
          decode_pool(workers), build_lane(workers > 1),
          decode_workers(workers), setup_id(id), antenna(ant),
          timeline(Tick(period_seconds) * ps + period_ps) {
        if (period_seconds < 0 || period_ps < 0 || period_ps >= ps ||
            timeline.period <= 0)
            throw std::invalid_argument("Expected a positive epoch period in "
                                        "exact seconds/picoseconds");
        if (protocol != "ubx" && protocol != "sbf")
            throw std::invalid_argument("Expected ubx or sbf");
        if (protocol == "ubx" && ant != 0)
            throw std::invalid_argument(
                "RAWX importer supports antenna 0 only");
    }
    std::array<std::shared_ptr<Batch>, 4>
    feed(std::span<const uint8_t> data, bool finalize_telemetry = false) {
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock())
            throw std::runtime_error("Concurrent importer use");
        auto out = observations();
        auto ev = events();
        auto raw = raw_bits();
        auto receiver = telemetry_schema();
        std::array batches{out, ev, raw, receiver};
        for (size_t i = 0; i < batches.size(); ++i)
            Batch::reserve(batches[i]->array, capacity_hints[i]);
        std::vector<ObservationOutput> completed_observations;
        std::vector<RawOutput> completed_bits;
        std::deque<std::future<void>> builds;
        auto flush_build = [&] {
            if (completed_observations.empty() && completed_bits.empty())
                return;
            if (builds.size() >= 2) {
                builds.front().get();
                builds.pop_front();
            }
            builds.push_back(build_lane.submit(std::packaged_task<void()>(
                [out, raw, id = setup_id, ant = antenna,
                 observations = std::move(completed_observations),
                 bits = std::move(completed_bits)] {
                    std::vector<const neognss_obs::Measurement *> selected;
                    for (const auto &epoch : observations) {
                        selected.clear();
                        for (const auto &m : epoch.rows)
                            if (m.antenna == ant)
                                selected.push_back(&m);
                        append_epoch(*out, selected, epoch.time, id);
                    }
                    for (const auto &record : bits)
                        append_bits(*raw, record.time, id, record.bits,
                                    record.uptime, record.archive_day);
                })));
            completed_observations.clear();
            completed_bits.clear();
        };
        auto emit = [&](neognss_obs::Measurements &e) {
            Tick t;
            try {
                t = time_of(e);
            } catch (const std::runtime_error &) {
                ++untimed;
                return;
            }
            ++epochs;
            complete(*ev, t, setup_id, !e.tow_ms.has_value());

            for (auto &m : e.rows)
                if (m.antenna != antenna)
                    ++other_antenna;
            completed_observations.push_back({t, std::move(e.rows)});
        };
        auto consume = [&](neognss_obs::CnexDecodedFrame &item) {
            if (item.error)
                std::rethrow_exception(item.error);
            const auto &f = item.frame;
            if (f.protocol() == cppgnss::Protocol::sbf && f.id() >= 4109 &&
                f.id() <= 4113)
                ++meas3;
            auto &e = item.measurements;
            navigation(f, completed_bits, *ev, e, item.bits);
            if (f.offset >= nav_skip_before)
                telemetry.frame(f, timeline.navigation, timeline.archive_day);
            if (f.offset >= nav_skip_before &&
                f.protocol() == cppgnss::Protocol::ubx && f.id() == 0x0120 &&
                telemetry.current.contains("receiver_uptime_s") &&
                !telemetry.current.value("gpst", Json(nullptr)).is_null()) {
                const auto u =
                    telemetry.current.value("receiver_uptime_s", Json(nullptr));
                if (!u.is_null()) {
                    Tick uptime =
                        Tick(u[0].get<int64_t>()) * ps + u[1].get<int64_t>();
                    auto sample = timeline.navigation;
                    // The assembler explicitly paired MON-SYS with this PVT
                    // cycle; never compare it against the previous NAV anchor.
                    auto reason = timeline.report_uptime(uptime, sample, true);
                    restart(reason, sample, uptime, *ev);
                    if (reason != neognss_obs::ReceiverTime::Restart::none)
                        timeline.set_navigation(sample);
                }
            }
            status(f, *ev);
            estimates_frame(f);
            if (e && f.offset >= nav_skip_before)
                telemetry.measurement(*e);
            telemetry.bound();
            auto &extra = item.extras;
            auto adopt = [&](const neognss_obs::Measurements &time) {
                if (!pending || pending->week != time.week ||
                    pending->tow_ms != time.tow_ms) {
                    if (has_measurements)
                        ++incomplete;
                    extra_unmatched += extras.size();
                    extras.clear();
                    has_measurements = false;
                    pending = neognss_obs::Measurements{};
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
                if (f.protocol() == cppgnss::Protocol::ubx) {
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
            if (f.protocol() == cppgnss::Protocol::sbf && f.id() == 5922 &&
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
        };
        // Own complete wire bytes, never retain StreamDecoder's callback spans.
        // Bound decoded temporaries independently of Python's input chunk size.
        std::vector<uint8_t> wire;
        std::vector<neognss_obs::CnexDecodedFrame> frames;
        std::vector<size_t> offsets;
        auto flush = [&] {
            for (size_t i = 0; i < frames.size(); ++i) {
                auto &f = frames[i].frame;
                const auto length = f.wire.size();
                f.wire =
                    std::span<const uint8_t>(wire).subspan(offsets[i], length);
                f.payload = f.wire.subspan(
                    f.protocol() == cppgnss::Protocol::ubx ? 6 : 8, length - 8);
            }
            decode_pool.run(frames);
            for (auto &item : frames)
                consume(item);
            flush_build();
            frames.clear();
            offsets.clear();
            wire.clear();
        };
        size_t serial_frames = 0;
        decoder.feed(data, [&](const cppgnss::FrameView &f) {
            if (decode_workers == 1) {
                neognss_obs::CnexDecodedFrame item{f};
                item.decode();
                consume(item);
                if (++serial_frames == 2048) {
                    flush_build();
                    serial_frames = 0;
                }
                return;
            }
            offsets.push_back(wire.size());
            wire.insert(wire.end(), f.wire.begin(), f.wire.end());
            frames.push_back({f});
            if (frames.size() >= 2048 || wire.size() >= 1024 * 1024)
                flush();
        });
        flush();
        for (auto &job : builds)
            job.get();
        out->finish();
        ev->finish();
        raw->finish();
        if (finalize_telemetry)
            telemetry.finish();
        telemetry.write(*receiver, setup_id);
        receiver->finish();
        for (size_t i = 0; i < batches.size(); ++i)
            capacity_hints[i] =
                Batch::sizes(batches[i]->array, batches[i]->schema);
        return batches;
    }
    Json checkpoint() {
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock())
            throw std::runtime_error("Concurrent importer use");
        Json state = Json::object();
        state["nav_ms"] = nav_ms ? Json(*nav_ms) : Json(nullptr);
        state["nav_conflict"] = nav_conflict;
        state["closure_ms"] = closure_ms ? Json(*closure_ms) : Json(nullptr);
        state["time_policy"] = 4;
        state["telemetry"] = telemetry.save();
        auto save_time = [&](const char *key, std::optional<Tick> t) {
            state[key] = t ? time_parts(*t) : Json(nullptr);
        };
        save_time("navigation", timeline.navigation);
        save_time("closure_time", closure_time);
        save_time("progress_time", timeline.progress);
        save_time("uptime", timeline.uptime);
        save_time("anchor_uptime", timeline.anchor_uptime);
        save_time("uptime_gpst", timeline.uptime_gpst);
        save_time("paired_offset", timeline.paired_offset);
        state["archive_day"] = timeline.archive_day;
        state["new_navigation"] = new_navigation;
        Json times = Json::object();
        for (const auto &[axis, time] : last_times)
            times[axis.c_str()] = time_parts(time);
        state["last_times"] = times;
        auto offset = pending ? pending_start : decoder.pending_offset();
        state["skip_bytes"] = decoder.pending_offset() - offset;
        return state;
    }
    void restore(const Json &state) {
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock())
            throw std::runtime_error("Concurrent importer use");
        if (decoder.bytes)
            throw std::runtime_error("Restore before feeding input");
        nav_ms = (state["nav_ms"].is_null() ? std::optional<int64_t>{}
                                            : state["nav_ms"].get<int64_t>());
        nav_conflict = state["nav_conflict"].get<bool>();
        closure_ms = (state["closure_ms"].is_null()
                          ? std::optional<int64_t>{}
                          : state["closure_ms"].get<int64_t>());
        if (!state.contains("time_policy") ||
            state["time_policy"].get<int>() != 4)
            throw std::runtime_error(
                "Old receiver-time checkpoint; rebuild import");
        telemetry.restore(state.at("telemetry"));
        auto load_time = [&](const char *key) -> std::optional<Tick> {
            if (state[key].is_null())
                return {};
            auto t = state[key].get<std::pair<int64_t, int64_t>>();
            if (t.second <= -ps || t.second >= ps)
                throw std::runtime_error("Invalid checkpoint time fraction");
            return Tick(t.first) * ps + t.second;
        };
        timeline.navigation = load_time("navigation");
        closure_time = load_time("closure_time");
        timeline.progress = load_time("progress_time");
        timeline.uptime = load_time("uptime");
        timeline.anchor_uptime = load_time("anchor_uptime");
        timeline.uptime_gpst = load_time("uptime_gpst");
        timeline.paired_offset = load_time("paired_offset");
        timeline.archive_day = state["archive_day"].get<int64_t>();
        new_navigation = state["new_navigation"].get<bool>();
        nav_skip_before = state["skip_bytes"].get<uint64_t>();
        for (const auto &[axis, value] : state["last_times"].items()) {
            auto seconds = value[0].get<int64_t>(),
                 fraction = value[1].get<int64_t>();
            if (seconds < 0 || fraction < 0 || fraction >= ps)
                throw std::runtime_error("Invalid checkpoint GPST");
            last_times[axis] = Tick(seconds) * ps + fraction;
        }
    }
    Json summary() {
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock())
            throw std::runtime_error("Concurrent importer use");
        Json d = Json::object();
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
        if (auto day = telemetry.safe_day())
            safe_day = safe_day ? std::min(*safe_day, *day) : day;
        d["telemetry_partial_rows"] = telemetry.partial_rows;
        d["telemetry_pending"] = telemetry.has_pending();
        d["safe_day"] = safe_day ? Json(*safe_day) : Json(nullptr);
        d["cursor_day"] = cursor_day.value_or(timeline.archive_day);
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
        d["receiver_restarts"] = restarts;
        return d;
    }
};
} // namespace neognss_obs::cnex_detail

namespace neognss_obs {
CnexBatch::~CnexBatch() {
    if (array.release)
        array.release(&array);
    if (schema.release)
        schema.release(&schema);
}
struct CnexEngine::Impl : cnex_detail::Reader {
    using Reader::Reader;
};
CnexEngine::CnexEngine(const std::string &p, const std::string &id, unsigned a,
                       int64_t s, int64_t ps, unsigned w)
    : impl_(std::make_unique<Impl>(p, id, a, s, ps, w)) {}
CnexEngine::~CnexEngine() = default;
CnexBatches CnexEngine::feed(std::span<const uint8_t> bytes) {
    auto source = impl_->feed(bytes);
    CnexBatches out;
    std::copy(source.begin(), source.end(), out.begin());
    return out;
}
CnexBatches CnexEngine::finish_telemetry() {
    auto batches = impl_->feed({}, true);
    CnexBatches result;
    std::copy(batches.begin(), batches.end(), result.begin());
    return result;
}
nlohmann::json CnexEngine::summary() { return impl_->summary(); }
nlohmann::json CnexEngine::checkpoint() { return impl_->checkpoint(); }
nlohmann::json CnexEngine::time_error() { return impl_->time_error(); }
void CnexEngine::restore(const nlohmann::json &s) { impl_->restore(s); }
struct CnexTimeProbe::Impl : cnex_detail::Probe {
    using Probe::Probe;
};
CnexTimeProbe::CnexTimeProbe(const std::string &p)
    : impl_(std::make_unique<Impl>(p)) {}
CnexTimeProbe::~CnexTimeProbe() = default;
void CnexTimeProbe::feed(std::span<const uint8_t> b) { impl_->feed(b); }
nlohmann::json CnexTimeProbe::result() { return impl_->result(); }
} // namespace neognss_obs
